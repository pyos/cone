#!/usr/bin/env bash
root=obj/tests/deck
addrs=()
ports=()
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
    for ((i = 0; i < "${#children[@]}"; i++)); do
        mv "$root/port-${ports[i]}.stderr" "$root/${children[i]}.stderr"
    done
    exit $1
}

trap "finish_all 0" SIGINT
trap "finish_all 1" SIGTERM
for ((i = 0; i < $1; i++)); do
    port="3200${#addrs[@]}"
    ./main "$1" ":$port" "${addrs[@]}" >>"$root/mutex" 2>"$root/port-$port.stderr" &
    echo "port $port = pid $!"
    sleep 0.1
    addrs=("${addrs[@]}" "127.0.0.1:$port")
    ports=("${ports[@]}" "$port")
    children=("${children[@]}" "$!")
done
echo "all nodes online, press Ctrl+C to stop"
while kill -0 "${children[@]}" 2>/dev/null; do sleep 1; done
finish_all 1
