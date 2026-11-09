#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$root/config/wine-source.env"
destination="$root/sources/wine"

if [[ -e "$destination" ]]; then
    if [[ ! -d "$destination/.git" ]]; then
        printf '已有目录不是预期的 Wine Git checkout：%s\n' "$destination" >&2
        exit 1
    fi
    actual=$(git -C "$destination" rev-parse HEAD)
    if [[ "$actual" != "$WINE_COMMIT" ]]; then
        printf '已有 checkout 的 HEAD 为 %s，基线为 %s；保留现有工作，不自动重置。\n' \
            "$actual" "$WINE_COMMIT" >&2
        exit 1
    fi
    printf 'Wine %s 基线已就绪，保留现有分支和工作区修改。\n' "$WINE_VERSION"
    git -C "$destination" status --short --branch
    exit 0
fi

mkdir -p "$root/sources"
temporary=$(mktemp -d "$root/sources/.wine-fetch.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT
git_options=()
if [[ -n "${WINE_GIT_PROXY:-}" ]]; then
    git_options=(-c "http.proxy=$WINE_GIT_PROXY" -c "https.proxy=$WINE_GIT_PROXY")
fi
git "${git_options[@]}" clone --depth=1 --branch "$WINE_TAG" "$WINE_REPOSITORY" "$temporary"
actual=$(git -C "$temporary" rev-parse HEAD)
if [[ "$actual" != "$WINE_COMMIT" ]]; then
    printf '上游标签与锁定提交不符：%s\n' "$actual" >&2
    exit 1
fi
git -C "$temporary" switch -c "winetk/$WINE_VERSION"
mv -T -- "$temporary" "$destination"
trap - EXIT
printf 'Wine %s 已准备：%s\n' "$WINE_VERSION" "$destination"
