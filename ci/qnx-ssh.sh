#!/bin/bash
# Helpers for the QEMU guest started by qemu-up.sh (default image user qnxuser/qnxuser).
O="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
case "$1" in
  run) shift; sshpass -p qnxuser ssh $O -p 2222 qnxuser@127.0.0.1 "$@" ;;
  put) shift; dest=${@: -1}; sshpass -p qnxuser scp $O -P 2222 "${@:1:$#-1}" qnxuser@127.0.0.1:"$dest" ;;
  get) shift; sshpass -p qnxuser scp $O -P 2222 qnxuser@127.0.0.1:"$1" "$2" ;;
esac
