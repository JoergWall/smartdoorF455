#!/bin/bash
# run this script like this
# ./run_smartdoorF455.sh
# script decouples smartdoorF455 from shell and logs errors to logfile
# creates ~/log directory in home directory, if not there already
logfile=$(date +%Y%m%d\_%H%M%S)\_smartdoorF455.log
USER_HOME=$(eval echo ~${USER}) # not SUDO_USER, no SUDO required with WiringPi
logdir=$USER_HOME/log
progdir=$USER_HOME/smartdoorF455/bin
prog=smartdoorF455
# cd to $progdir, as config file is located in relative path ../src
cd $progdir
# check for log directory
if [ ! -d $logdir ] 
then
    mkdir $logdir
    chown $SUDO_USER.$SUDO_GROUP $logdir
    echo "$logdir created"
fi
# check for executable program file
if [ ! -x $progdir/$prog ]
then
    echo "please first build $prog"
    echo "check README.md"
    exit
fi
# check if running already
process_id=$(pgrep -x $prog)
if [ -z "$process_id" ]; then
    echo "Process $prog not running."
else
    echo "$prog already running with process id $process_id"
    exit
fi
# check, if nice capability has been set to executable program
# if not, try to set the capability with sudo setcap command
nice_capability=$(setcap -v 'cap_sys_nice=eip' $progdir/$prog)
nice_OK=${nice_capability: -2} # get last two characters of string
echo "nice: $nice_OK"
if [ "$nice_OK" != "OK" ]; then
    echo "trying to set nice capability with this command:"
    echo "sudo setcap 'cap_sys_nice=eip' $progdir/$prog"
    sudo setcap 'cap_sys_nice=eip' $progdir/$prog
fi
# run $prog with nohup to decouple from shell
nohup $progdir/$prog </dev/null > $logdir/$logfile 2>&1 &
PROG_PID=$(pgrep -x $prog)
echo "$prog started successfully with PID $PROG_PID"
echo "watch $logdir/$logfile for errors"
# obtain process id to set CPU affinity to CPU #3
# assuming that isolcpus=3 as been added to /boot/cmdline.txt
sudo taskset -cp 3 $PROG_PID
