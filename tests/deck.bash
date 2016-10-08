#!/usr/bin/env bash
root=obj/tests/deck
addrs=()
children=()

if [ "$#" != "1" ]; then
    echo "usage: $0 N"
    exit 2
fi

rm -rf "$root"
mkdir -p "$root"
touch "$root/mutex"

function finish_all() {
    kill "${children[@]}" 2>/dev/null
    exit $1
}

trap "finish_all -2" SIGINT
trap "finish_all -15" SIGTERM
for ((i=1; i<=$1; i++)); do
    port="3200${#addrs[@]}"
    ./main "$1" ":$port" "${addrs[@]}" >"$root/mutex" 2>"$root/$port.stderr" &
    echo ":$port = $!"
    sleep 0.1
    addrs=("${addrs[@]}" "127.0.0.1:$port")
    children=("${children[@]}" "$!")
done
for ((i=1; i<="${#children[@]}"; i++)); do wait -n "${children[@]}" || finish_all $?; done
