#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/cxl-global.sh" || exit

# Validate args BEFORE any side effect: bring_all_cpus_online below re-populates
# node 1, so aborting after it would leave the CXL topology destroyed.
if [[ $# != 4 ]]; then
	echo "Usage: sudo ./modify-uncore-freq.sh [node0-min] [node0-max] [node1-min] [node1-max]"
	exit 1
fi

echo "setting to base conf ..."
check_base_conf
echo "setting all cores online ..."
bring_all_cpus_online

ZERO_MIN_UNCORE_FREQ=$1
ZERO_MAX_UNCORE_FREQ=$2
ONE_MIN_UNCORE_FREQ=$3
ONE_MAX_UNCORE_FREQ=$4

# Change node 0 min uncore frequency
change_node_zero_min()
{
	local freq=$ZERO_MIN_UNCORE_FREQ
	# echo "Change node 0 min uncore frequency to $freq"
	local curfreq=$( cat /sys/devices/system/cpu/intel_uncore_frequency/package_00_die_00/min_freq_khz)
	# echo "Current node 0 min uncore frequency: $curfreq"
	echo $freq > /sys/devices/system/cpu/intel_uncore_frequency/package_00_die_00/min_freq_khz
	local curfreq=$( cat /sys/devices/system/cpu/intel_uncore_frequency/package_00_die_00/min_freq_khz)
	if [[ $freq == $curfreq ]]; then
		echo "Success! Node 0 min uncore frequency has been set to $curfreq"
	else
		echo "Fail! Current node 0 min uncore frequency: $curfreq"
	fi
}

# Change node 0 max uncore frequency
change_node_zero_max()
{
	local freq=$ZERO_MAX_UNCORE_FREQ
	local curfreq=$( cat /sys/devices/system/cpu/intel_uncore_frequency/package_00_die_00/max_freq_khz)
	echo $freq > /sys/devices/system/cpu/intel_uncore_frequency/package_00_die_00/max_freq_khz
	local curfreq=$( cat /sys/devices/system/cpu/intel_uncore_frequency/package_00_die_00/max_freq_khz)
	if [[ $freq == $curfreq ]]; then
		echo "Success! Node 0 max uncore frequency has been set to $curfreq"
	else
		echo "Fail! Current node 0 max uncore frequency: $curfreq"
	fi
}

# Change node 1 min uncore frequency
change_node_one_min()
{
	if [[ ! -e ${NODE_ONE_UNCORE_DIR}/min_freq_khz ]]; then
		echo "Skip node 1 min uncore: package_01_die_00 is absent"
		return 0
	fi
	local freq=$ONE_MIN_UNCORE_FREQ
	local curfreq=$( cat ${NODE_ONE_UNCORE_DIR}/min_freq_khz)
	echo $freq > ${NODE_ONE_UNCORE_DIR}/min_freq_khz
	local curfreq=$( cat ${NODE_ONE_UNCORE_DIR}/min_freq_khz)
	if [[ $freq == $curfreq ]]; then
		echo "Success! Node 1 min uncore frequency has been set to $curfreq"
	else
		echo "Fail! Current node 1 min uncore frequency: $curfreq"
	fi
}

# Change node 1 max uncore frequency
change_node_one_max()
{
	if [[ ! -e ${NODE_ONE_UNCORE_DIR}/max_freq_khz ]]; then
		echo "Skip node 1 max uncore: package_01_die_00 is absent"
		return 0
	fi
	local freq=$ONE_MAX_UNCORE_FREQ
	local curfreq=$( cat ${NODE_ONE_UNCORE_DIR}/max_freq_khz)
	echo $freq > ${NODE_ONE_UNCORE_DIR}/max_freq_khz
	local curfreq=$( cat ${NODE_ONE_UNCORE_DIR}/max_freq_khz)
	if [[ $freq == $curfreq ]]; then
		echo "Success! Node 1 max uncore frequency has been set to $curfreq"
	else
		echo "Fail! Current node 1 max uncore frequency: $curfreq"
	fi
}

NODE_ONE_UNCORE_DIR=/sys/devices/system/cpu/intel_uncore_frequency/package_01_die_00
for candidate in /sys/devices/system/cpu/intel_uncore_frequency/package_01_die_*; do
    if [[ -e "$candidate/min_freq_khz" ]]; then NODE_ONE_UNCORE_DIR="$candidate"; break; fi
done
main()
{
	change_node_zero_min
	change_node_zero_max
	change_node_one_min
	change_node_one_max
}

main
echo "./modify-uncore-freq.sh DONE"
exit
