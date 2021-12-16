#!/bin/bash
# run this script with sudo like this
# sudo run_smartdoorF455_c.sh
# script decouples smartdoorF455 from shell and logs errors to logfile
# creates ~/log directory in home directory, if not there already
logfile=$(date +%Y%m%d\_%H%M%S)\_smartdoorF455.log
USER_HOME=$(eval echo ~${SUDO_USER})
logdir=$USER_HOME/log
progdir=$USER_HOME/smartdoorF455/bin
prog=smartdoorF455_c
# check if run by sudo
if [ "$EUID" -ne 0 ]
then 
    echo "Please run with sudo or as root"
    exit
fi
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
if pgrep -x $prog
then 
    echo "$prog already running"
    exit
fi
# run $prog with nohup to decouple from shell
nohup $progdir/$prog </dev/null > $logdir/$logfile 2>&1 &
echo "$prog started successfully"
echo "watch $logdir/$logfile for errors"
