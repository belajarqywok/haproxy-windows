#!/bin/sh

_help()
{
  cat << EOF
### run-regtests.sh ###
  Running run-regtests.sh --help shows this information about how to use it

  Run without parameters to run all tests in the current folder (including subfolders)
    run-regtests.sh

  Provide paths to run tests from (including subfolders):
    run-regtests.sh ./tests1 ./tests2

  Parameters:
    --j <NUM>, To run vtest with multiple jobs / threads for a faster overall result
      run-regtests.sh ./fasttest --j 16

    --v, to run verbose
      run-regtests.sh --v, disables the default vtest 'quiet' parameter

    --debug to show test logs on standard output (implies --v)
      run-regtests.sh --debug

    --keep-logs to keep all log directories (by default kept if test fails)
      run-regtests.sh --keep-logs

    --vtestparams <ARGS>, passes custom ARGS to vtest
      run-regtests.sh --vtestparams "-n 10"

    --type <reg tests types> filter the types of the tests to be run, depending on
      the commented REGTESTS_TYPE variable value in each VTC file.
      The value of REGTESTS_TYPE supported are: default, slow, bug, broken, devel
      and experimental. When not specified, it is set to 'default' as default value.

      run-regtest.sh --type slow,default

    --clean to cleanup previous reg-tests log directories and exit
      run-regtests.sh --clean

  Including text below into a .vtc file will check for its requirements
  related to haproxy's target and compilation options
    # Below targets are not capable of completing this test successfully
    #EXCLUDE_TARGET=freebsd, abns sockets are not available on freebsd

    #EXCLUDE_TARGETS=dos,freebsd,windows

    # Below option is required to complete this test successfully
    #REQUIRE_OPTION=OPENSSL, this test needs OPENSSL compiled in.
    #REQUIRE_OPTIONS=ZLIB|SLZ,OPENSSL,LUA

    #REQUIRE_SERVICE=prometheus-exporter
    #REQUIRE_SERVICES=prometheus-exporter,foo

    # To define a range of versions that a test can run with:
    #REQUIRE_VERSION=0.0
    #REQUIRE_VERSION_BELOW=99.9

  Configure environment variables to set the haproxy and vtest binaries to use
    setenv HAPROXY_PROGRAM /usr/local/sbin/haproxy
    setenv VTEST_PROGRAM /usr/local/bin/vtest
    setenv HAPROXY_ARGS "-dM -de -m 50"
  or
    export HAPROXY_PROGRAM=/usr/local/sbin/haproxy
    export VTEST_PROGRAM=/usr/local/bin/vtest
    export HAPROXY_ARGS="-dM -de -m 50"
EOF
  exit 0
}

add_range_to_test_list()
{
    level0="*.vtc"
    level1="h*.vtc"
    level2="s*.vtc"
    level3="l*.vtc"
    level4="b*.vtc"
    level5="k*.vtc"
    level6="e*.vtc"

    new_range=$(echo $1 | tr '-' ' ')
    non_digit=$(echo $new_range | grep '[^0-9 ]')
    if [ -n "$non_digit" ] ; then
        return
    fi
    if [ "$new_range" = "$1" ] ; then
        if [ $1 -gt 6 ] ; then
            return
        fi
        eval echo '$'level$1
        return
    fi
    if [ -z "$new_range" ] ; then
        return
    fi
    list=
    for l in $(seq $new_range) ; do
        if [ -n "l" ] ; then
            if [ -z "$list" ] ; then
                list="$(eval echo '$'level${l})"
            else
                list="$list $(eval echo '$'level${l})"
            fi
        fi
    done

    echo $list
}

_startswith() {
  _str="$1"
  _sub="$2"
  echo "$_str" | grep "^$_sub" >/dev/null 2>&1
}

_findtests() {
  set -f

  REGTESTS_TYPES="${REGTESTS_TYPES:-default,bug,devel,slow}"
  any_test=$(echo $REGTESTS_TYPES | grep -cw "any")
  for i in $( find "$1" -name *.vtc ); do
    skiptest=
    OLDIFS="$IFS"; IFS="$LINEFEED"
    set -- $(grep '^#[0-9A-Z_]*=' "$i")
    IFS="$OLDIFS"

    require_version=""; require_version_below=""; require_options="";
    require_services=""; exclude_targets=""; regtest_type=""
    requiredoption=""; requiredservice=""; excludedtarget="";

    while [ $# -gt 0 ]; do
      v="$1"; v="${v#*=}"
      case "$1" in
        "#REQUIRE_VERSION="*)       require_version="$v" ;;
        "#REQUIRE_VERSION_BELOW="*) require_version_below="$v" ;;
        "#REQUIRE_OPTIONS="*)       require_options="$v" ;;
        "#REQUIRE_SERVICES="*)      require_services="$v" ;;
        "#EXCLUDE_TARGETS="*)       exclude_targets="$v" ;;
        "#REGTEST_TYPE="*)          regtest_type="$v" ;;
        "#REQUIRE_OPTION="*)        requiredoption="${v%,*}" ;;
        "#REQUIRE_SERVICE="*)       required_service="${v%,*}" ;;
        "#EXCLUDE_TARGET="*)        excludedtarget="${v%,*}" ;;
        # Note: any new variable declared here must be initialized above.
      esac
      shift
    done

    if [ $any_test -ne 1 ] ; then
        if [ -z $regtest_type ] ; then
            regtest_type=default
        fi
        if ! $(echo $REGTESTS_TYPES | grep -wq $regtest_type) ; then
            echo "  Skip $i because its type '"$regtest_type"' is excluded"
            skiptest=1
        fi
    fi

    if [ -n "$requiredoption" ]; then
      require_options="$require_options,$requiredoption"
    fi

    if [ -n "$requiredservice" ]; then
      require_services="$require_services,$requiredservice"
    fi

    if [ -n "$excludedtarget" ]; then
      exclude_targets="$exclude_targets,$excludedtarget"
    fi

    IFS=","; set -- $require_options;  IFS=$OLDIFS; require_options="$*"
    IFS=","; set -- $require_services; IFS=$OLDIFS; require_services="$*"
    IFS=","; set -- $exclude_targets;  IFS=$OLDIFS; exclude_targets="$*"

    if [ -n "$require_version" ]; then
      if [ $(_version "$HAPROXY_VERSION") -lt $(_version "$require_version") ]; then
        echo "  Skip $i because option haproxy is version: $HAPROXY_VERSION"
        echo "    REASON: this test requires at least version: $require_version"
        skiptest=1
      fi
    fi
    if [ -n "$require_version_below" ]; then
      if [ $(_version "$HAPROXY_VERSION") -ge $(_version "$require_version_below") ]; then
        echo "  Skip $i because option haproxy is version: $HAPROXY_VERSION"
        echo "    REASON: this test requires a version below: $require_version_below"
        skiptest=1
      fi
    fi

    for excludedtarget in $exclude_targets; do
      if [ "$excludedtarget" = "$TARGET" ]; then
        echo "  Skip $i because haproxy is compiled for the excluded target $TARGET"
        skiptest=1
      fi
    done

    for requiredoption in $require_options; do
      IFS="|"; set -- $requiredoption;  IFS=$OLDIFS; alternatives="$*"
      found=
      for alt in $alternatives; do
        if [ -z "${FEATURES_PATTERN##* +$alt *}" ]; then
          found=1;
	fi
      done
      if [ -z $found ]; then
        echo "  Skip $i because haproxy is not compiled with the required option $requiredoption"
        skiptest=1
      fi
    done

    for requiredservice in $require_services; do
      IFS="|"; set -- $requiredservice;  IFS=$OLDIFS; alternatives="$*"
      found=
      for alt in $alternatives; do
        if [ -z "${SERVICES_PATTERN##* $alt *}" ]; then
          found=1;
	fi
      done
      if [ -z $found ]; then
        echo "  Skip $i because haproxy is not compiled with the required service $requiredservice"
        skiptest=1
      fi
    done

    if [ -z $skiptest ]; then
      echo "  Add test: $i"
      testlist="$testlist $i"
    fi
  done
}

_cleanup()
{
  DIRS=$(find "${TESTDIR}" -maxdepth 1 -type d -name "haregtests-*" -exec basename {} \; 2>/dev/null)
  if [ -z "${DIRS}" ]; then
    echo "No reg-tests log directory found"
  else
    echo "Cleanup following reg-tests log directories:"
    for d in ${DIRS}; do
      echo  "    o ${TESTDIR}/$d"
    done
    read -p "Continue (y/n)?" reply
    case "$reply" in
      y|Y)
        for d in ${DIRS}; do
          rm -r "${TESTDIR}/$d"
        done
        echo "done"
        exit 0
        ;;
       *)
        echo "aborted"
        exit 1
        ;;
    esac
  fi
}


_process() {
  while [ ${#} -gt 0 ]; do
    if _startswith "$1" "-"; then
      case "${1}" in
        --j)
          jobcount="$2"
          shift
          ;;
        --vtestparams)
          vtestparams="$2"
          shift
          ;;
        --v)
          verbose=""
          ;;
        --debug)
          verbose=""
          debug="-v"
          ;;
        --keep-logs)
          keep_logs="-L"
          ;;
        --type)
	      REGTESTS_TYPES="$2"
	      shift
	      ;;
        --clean)
          _cleanup
          exit 0
          ;;
        --help)
          _help
          ;;
        *)
          echo "Unknown parameter : $1"
          exit 1
          ;;
      esac
    else
      REGTESTS="${REGTESTS} $1"
    fi
    shift 1
  done
}

# compute a version from up to 4 sub-version components, each multiplied
# by a power of 1000, and padded left with 0, 1 or 2 zeroes.
_version() {
  OLDIFS="$IFS"; IFS="."; set -- $*; IFS="$OLDIFS"
  set -- ${1%%[!0-9]*} 000${2%%[!0-9]*} 000${3%%[!0-9]*} 000${4%%[!0-9]*}
  prf2=${2%???}; prf3=${3%???}; prf4=${4%???}
  echo ${1}${2#$prf2}${3#$prf3}${4#$prf4}
}


HAPROXY_PROGRAM="${HAPROXY_PROGRAM:-${PWD}/haproxy}"
HAPROXY_ARGS="${HAPROXY_ARGS--dM}"
VTEST_PROGRAM="${VTEST_PROGRAM:-vtest}"
TESTDIR="${TMPDIR:-/tmp}"
REGTESTS=""
LINEFEED="
"

jobcount=""
verbose="-q"
debug=""
keep_logs="-l"
testlist=""

_process "$@";

echo ""
echo "########################## Preparing to run tests ##########################"

preparefailed=
if ! [ -x "$(command -v $HAPROXY_PROGRAM)" ]; then
  echo "haproxy not found in path, please specify HAPROXY_PROGRAM environment variable"
  preparefailed=1
fi
if ! [ -x "$(command -v $VTEST_PROGRAM)" ]; then
  echo "vtest not found in path, please specify VTEST_PROGRAM environment variable"
  preparefailed=1
fi
if [ $preparefailed ]; then
  exit 1
fi

{ read HAPROXY_VERSION; read TARGET; read FEATURES; read SERVICES; } << EOF
$($HAPROXY_PROGRAM $HAPROXY_ARGS -vv | grep 'HA-\?Proxy version\|TARGET.*=\|^Feature\|^Available services' | sed 's/.* [:=] //')
EOF

HAPROXY_VERSION=$(echo $HAPROXY_VERSION | cut -d " " -f 3)
echo "Testing with haproxy version: $HAPROXY_VERSION"

PROJECT_VERSION=$(${MAKE:-make} version 2>&1 | grep '^VERSION:\|^SUBVERS:'|cut -f2 -d' '|tr -d '\012')
if [ -z "${PROJECT_VERSION}${MAKE}" ]; then
        # try again with gmake, just in case
        PROJECT_VERSION=$(gmake version 2>&1 | grep '^VERSION:\|^SUBVERS:'|cut -f2 -d' '|tr -d '\012')
fi

FEATURES_PATTERN=" $FEATURES "
SERVICES_PATTERN=" $SERVICES "

TESTRUNDATETIME="$(date '+%Y-%m-%d_%H-%M-%S')"

mkdir -p "$TESTDIR" || exit 1
TESTDIR=$(mktemp -d "$TESTDIR/haregtests-$TESTRUNDATETIME.XXXXXX") || exit 1

export TMPDIR="$TESTDIR"
export HAPROXY_PROGRAM="$HAPROXY_PROGRAM"
if [ -n "$HAPROXY_ARGS" ]; then
   export HAPROXY_ARGS
fi

echo "Target : $TARGET"
echo "Options : $FEATURES"
echo "Services : $SERVICES"

echo "########################## Gathering tests to run ##########################"

if [ -z "$REGTESTS" ]; then
  _findtests reg-tests/
else
  for t in $REGTESTS; do
    _findtests $t
  done
fi

echo "########################## Starting vtest ##########################"
echo "Testing with haproxy version: $HAPROXY_VERSION"

if [ -n "$PROJECT_VERSION" -a "$PROJECT_VERSION" != "$HAPROXY_VERSION" ]; then
        echo "Warning: version does not match the current tree ($PROJECT_VERSION)"
fi

_vtresult=0
if [ -n "$testlist" ]; then
  if [ -n "$jobcount" ]; then
    jobcount="-j $jobcount"
  fi
  cmd="$VTEST_PROGRAM -b $((2<<20)) -k -t 10 $keep_logs $verbose $debug $jobcount $vtestparams $testlist"
  eval $cmd
  _vtresult=$?
else
  echo "No tests found that meet the required criteria"
fi


if [ $_vtresult -eq 0 ]; then
  # all tests were successful, removing tempdir (the last part.)
  # ignore errors is the directory is not empty or if it does not exist
   rmdir "$TESTDIR" 2>/dev/null
fi

if [ -d "${TESTDIR}" ]; then
  echo "########################## Gathering results ##########################"
  export TESTDIR
  find "$TESTDIR" -type d -name "vtc.*" -exec sh -c 'for i; do
    if [ ! -e "$i/LOG" ] ; then continue; fi

    cat <<- EOF | tee -a "$TESTDIR/failedtests.log"
$(echo "###### $(cat "$i/INFO") ######")
$(echo "## test results in: \"$i\"")
$(grep -E -- "^(----|\*    diag)" "$i/LOG")
EOF
  done' sh {} +
fi

exit $_vtresult
