#!/bin/sh

MBUNI=/usr/local/mbuni
BINDIR=$MBUNI/bin
PIDFILES=$MBUNI/run
CONF=$MBUNI/etc/mmsc.conf

MP=mmsproxy
MR=mmsrelay

LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$MBUNI/lib/mbuni

export LD_LIBRARY_PATH

test -x $BINDIR/$MP && test -x $BINDIR/$MR || exit 0

case "$1" in
  start)
    echo -n "Starting MMSC: mmsproxy"
    $BINDIR/$MP --daemonize --parachute --pid-file $PIDFILES/$MP.pid -- $CONF
    sleep 1
    echo -n " mmsrelay"
    $BINDIR/$MR --daemonize --parachute --pid-file $PIDFILES/$MR.pid -- $CONF
    echo "."
    ;;

  stop)
    echo -n "Stopping MMSC: mmsrelay"
    kill `cat $PIDFILES/$MR.pid`
    sleep 1
    echo -n " mmsproxy"
    kill `cat $PIDFILES/$MP.pid`
    echo "."
    ;;

  reload)
    # We don't have support for this yet.
    exit 1
    ;;

  restart|force-reload)
    $0 stop
    sleep 1
    $0 start
    ;;

  *)
    echo "Usage: $0 {start|stop|reload|restart|force-reload}"
    exit 1

esac

exit 0
