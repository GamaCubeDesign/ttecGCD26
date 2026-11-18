#!/usr/bin/env bash
# avaliar.sh — the ground's estimator against the truth of a real capture
# (tools/live_traffic), with and without the emitter category:
#
#   avaliar.sh CAPTURE_DIR [--pairs-db FILE]
#
# With the category is what the estimator could do with all the data; without
# it is what the ground really gets, since the satellite cannot downlink the
# category (the SBS feed has none). Each run is the estimator's own CLI and
# validation/avaliar_estimativas.py; the reports land in CAPTURE_DIR/avaliacao-local/
# (the local method alone) or CAPTURE_DIR/avaliacao-pares/ (with the pair method).

set -euo pipefail

dir=$(realpath "${1:?usage: avaliar.sh CAPTURE_DIR [--pairs-db FILE]}")
shift
pairs=(--no-pairs)
out=$dir/avaliacao-local
if [[ ${1:-} == --pairs-db ]]; then
    pairs=(--pairs-db "$(realpath "$2")")
    out=$dir/avaliacao-pares
fi
root=$(cd "$(dirname "$0")/../.." && pwd)
python=${PYTHON:-python3}
mkdir -p "$out"

# A real capture is rebuilt from its raw responses; a synthetic scenario
# (synthetic.py) already holds its capture and its truth.
if [[ -f $dir/raw.jsonl.gz ]]; then
    "$python" "$root/tools/live_traffic/live_traffic.py" convert "$dir" >/dev/null
    "$python" "$root/tools/live_traffic/live_traffic.py" convert "$dir" --no-category >/dev/null
    [[ -f $dir/ground_truth.json ]] || "$python" "$root/tools/live_traffic/live_traffic.py" truth "$dir" >/dev/null
fi

cd "$root/ground/host"
for kind in capture capture_sem_categoria; do
    tag=${kind#capture}; tag=${tag#_}; tag=${tag:-com_categoria}
    rm -f "$out/$tag.db" "$out/$tag.db-wal" "$out/$tag.db-shm"
    "$python" -m adsb.estimator --input "$dir/$kind.ndjson" --telemetry-db "$out/$tag.db" \
        --output "$out/$tag.estado.json" "${pairs[@]}" > "$out/$tag.estimador.txt" 2>&1
    "$python" adsb/validation/avaliar_estimativas.py --state "$out/$tag.estado.json" \
        --truth "$dir/ground_truth.json" --json-out "$out/$tag.json" --csv-out "$out/$tag.csv" \
        > "$out/$tag.txt"
    "$python" "$root/tools/live_traffic/avaliar_combinada.py" "$out/$tag.estado.json" \
        "$dir/ground_truth.json" --json "$out/$tag.combinada.json" > "$out/$tag.combinada.txt"
    echo "== $tag · método local"
    sed -n '/^RESUMO/,/^TOTAL/p' "$out/$tag.txt"
    echo "== $tag · ADR-0019"
    cat "$out/$tag.combinada.txt"
done
