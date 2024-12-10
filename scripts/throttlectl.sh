#!/usr/bin/bash

# SPDX-License-Identifier: GPL-2.0-or-later
# This script is called to either turn off or turn on RT throttling
# The 'off' argument causes the current values of the throttling
# parameters from /proc/sys/kernel/sched_rt_{period,runtime}_us to
# be saved and then rt throttling to be disabled.
# The 'on' argument causes the previously saved values to be restored
# or if those are not found the defaults are re-applied.

procpath=/proc/sys/kernel
savedir=/run/stalld
cmd=$1
defperiod=1000000
defruntime=950000

# set our default umask to create files with 600 mode
umask 077

# make sure the stalld run directory exists
if [[ ! -d ${savedir} ]]; then
    mkdir -p -m 0755 ${savedir}
fi

case ${cmd} in
    # turn off RT throttling and save previous values
    off)
	# get rid of any existing save files
	rm -rf ${savedir}/rtthrottle*
	runtime=$(cat ${procpath}/sched_rt_runtime_us)
	period=$(cat ${procpath}/sched_rt_period_us)
	savefile=$(mktemp -p ${savedir} rtthrottle-XXXXXXX)
	if [[ $? != 0 ]]; then
	    echo "Failed to make savefile"
	    exit 1
	fi
	chmod 600 ${savefile}
	echo "period=${period}" > ${savefile}
	echo "runtime=${runtime}" >> ${savefile}
	# don't do anything if it's already disabled
	if [[ "${runtime}" != "-1" ]]; then
	    echo -1 > ${procpath}/sched_rt_runtime_us
	fi
	# verify that we really turned it off
	if [[ "$(cat ${procpath}/sched_rt_runtime_us)" != "-1" ]]; then
	    logger -t stalld "failed to turn off RT throttling"
	    exit 1
	fi
	logger -t stalld "Disabled RT throttling"
	;;

    # turn on RT throttling and restore previous values
    on)
	savefile=${savedir}/rtthrottle-*
	# if the wildcard above matches more than one file
	# just restore to default values and nuke the matched files
	if [[ -f ${savefile} ]]; then
	    period=$(awk -F= '/^period/ {print $2}' ${savefile})
	    runtime=$(awk -F= '/^runtime/ {print $2}' ${savefile})
	else
	    period=${defperiod}
	    runtime=${defruntime}
	fi
	rm -f ${savefile}
	# restore the previous values and log that we did it
	echo $period > ${procpath}/sched_rt_period_us
	echo $runtime > ${procpath}/sched_rt_runtime_us
	logger -t stalld "Restored RT throttling"
    ;;
    show)
	echo "Period:  $(cat ${procpath}/sched_rt_period_us)"
	runtime=$(cat ${procpath}/sched_rt_runtime_us)
	echo "Runtime: ${runtime}"
	if [[ "${runtime}" == -1 ]]; then
	    echo "RT Throttling is disabled"
	else
	    echo "RT Throttling is enabled"
	fi
	;;
    *)
	echo "usage: $0 on|off|show"
	exit 0
	;;
esac
