#!/bin/sh
set -e

DATA_DIR="${DATA_DIR:-/data}"

download_pbf() {
    mkdir -p "$DATA_DIR/pbf"
    for url in $PBF_URLS; do
        filename=$(basename "$url")
        if [ ! -f "$DATA_DIR/pbf/$filename" ]; then
            echo "Downloading $url..."
            curl -fSL -o "$DATA_DIR/pbf/$filename" "$url"
        else
            echo "Already downloaded: $filename"
        fi
    done
}

build_index() {
    set -- "$DATA_DIR/index"
    for f in "$DATA_DIR"/pbf/*.osm.pbf; do
        [ -f "$f" ] && set -- "$@" "$f"
    done
    if [ "$#" -eq 1 ]; then
        echo "Error: no PBF files found in $DATA_DIR/pbf/"
        exit 1
    fi
    mkdir -p "$DATA_DIR/index"
    [ -n "$STREET_LEVEL" ] && set -- "$@" --street-level "$STREET_LEVEL"
    [ -n "$ADMIN_LEVEL" ] && set -- "$@" --admin-level "$ADMIN_LEVEL"
    echo "Building index..."
    build-index "$@"
    echo "Index built."
}

serve() {
    set -- "$DATA_DIR/index"
    if [ -n "$DOMAIN" ]; then
        set -- "$@" --domain "$DOMAIN"
        if [ -n "$CACHE_DIR" ]; then
            set -- "$@" --cache "$CACHE_DIR"
        fi
    else
        set -- "$@" "${BIND_ADDR:-0.0.0.0:3000}"
    fi
    [ -n "$STREET_LEVEL" ] && set -- "$@" --street-level "$STREET_LEVEL"
    [ -n "$ADMIN_LEVEL" ] && set -- "$@" --admin-level "$ADMIN_LEVEL"
    [ -n "$SEARCH_DISTANCE" ] && set -- "$@" --search-distance "$SEARCH_DISTANCE"
    echo "Starting server..."
    exec query-server "$@"
}

case "${1:-auto}" in
    build)
        download_pbf
        build_index
        ;;
    serve)
        serve
        ;;
    auto)
        download_pbf
        build_index
        serve
        ;;
    *)
        exec "$@"
        ;;
esac
