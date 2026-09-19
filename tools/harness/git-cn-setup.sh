#!/usr/bin/env bash
# Make GitHub workable from a China-based machine, and be honest about what each measure buys.
#
# Measured here (2026-09) rather than assumed:
#   - direct HTTPS  0.5-1.6 s      <- fastest READ path, no SSH handshake involved
#   - domestic mirrors (gh-proxy.com / ghfast.top / ghproxy.net) 1.7-2.2 s, reads only
#   - direct SSH    3.5-4.4 s      <- handshake-dominated; this is the WRITE path
#   - SSH connection multiplexing is NOT available: MSYS/Windows OpenSSH answers
#     "mux_client_request_session: read from master failed" and re-handshakes anyway
#   - dead ends seen today: hub.gitmirror.com (error), kkgithub.com (timeout)
#
# So the setup is a ladder, not a "proxy":
#   write : SSH only, batched (one push per work session).  No mirror ever sees a push or a key.
#   read  : `github-https` (direct HTTPS) first; `cn-mirror` (domestic accelerator) when the
#           direct route is down.  Both have their push URL pinned to the SSH origin, so neither
#           can carry content or a credential through a third party by accident.
#   ssh   : keepalives, plus `github-443` (same host keys, verified) for when port 22 is throttled.
#   proxy : opt-in.  A locally run proxy client is the only route that is actually stable from
#           China, so `--proxy <url>` wires the repo-local git config and the ssh ProxyCommand -
#           and refuses to write anything unless that port is already listening, because a config
#           pointing at a proxy that is not running turns every command into a hang.
#
# Usage:
#   bash tools/harness/git-cn-setup.sh                              # apply and verify (idempotent)
#   bash tools/harness/git-cn-setup.sh --status                     # report only
#   bash tools/harness/git-cn-setup.sh --proxy http://127.0.0.1:7890
#   bash tools/harness/git-cn-setup.sh --proxy socks5://127.0.0.1:1080
#   bash tools/harness/git-cn-setup.sh --proxy off
#   MIRROR=https://ghproxy.net bash tools/harness/git-cn-setup.sh
set -u

SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SELF_DIR" && git rev-parse --show-toplevel 2>/dev/null || echo "$SELF_DIR/../..")"
MIRROR="${MIRROR:-https://gh-proxy.com}"
SSH_DIR="$HOME/.ssh"
SSH_CFG="$SSH_DIR/config"
BLOCK_A="# >>> kynovo-cn-github (managed by tools/harness/git-cn-setup.sh - do not edit inside)"
BLOCK_B="# <<< kynovo-cn-github"

STATUS=0
PROXY_MODE=0
PROXY_URL=""
case "${1:-}" in
  --status) STATUS=1 ;;
  --proxy)
    PROXY_MODE=1
    PROXY_URL="${2:-}"
    if [ -z "$PROXY_URL" ]; then
      echo "usage: $0 --proxy <http://127.0.0.1:PORT | socks5://127.0.0.1:PORT | off>" >&2
      exit 2
    fi
    ;;
esac

say(){ printf '%s\n' "$*"; }
now_ms(){ date +%s%N; }
ms_since(){ echo $(( ( $(now_ms) - $1 ) / 1000000 )); }
gitq(){ git -C "$REPO_ROOT" "$@"; }

# One managed block for github.com, so the ProxyCommand can never end up after a block that
# would win the first-match rule.  ssh takes the FIRST value of each keyword it meets.
write_ssh_block(){
  # $1 = proxy spec: "" for none, else "connect -H host:port"
  proxy_line=""
  [ -n "$1" ] && proxy_line="  ProxyCommand $1 %h %p"
  cat <<EOF
$BLOCK_A
# Keepalives: a route that goes bad dies in ~45 s instead of hanging the operation.
# github-443 carries the same host keys as github.com (checked against api.github.com/meta):
#   git remote set-url origin git@github-443:<owner>/<repo>.git
# No ControlMaster here on purpose - this platform's OpenSSH does not support multiplexing.
Host github.com
  HostName github.com
  User git
$proxy_line
  ServerAliveInterval 15
  ServerAliveCountMax 3
  TCPKeepAlive yes

Host github-443
  HostName ssh.github.com
  Port 443
  User git
$proxy_line
  ServerAliveInterval 15
  ServerAliveCountMax 3
  TCPKeepAlive yes
$BLOCK_B
EOF
}

install_ssh_block(){
  # Replaces the managed block in place, or appends it when absent.
  content="$(write_ssh_block "${1:-}")"
  mkdir -p "$SSH_DIR"; chmod 700 "$SSH_DIR" 2>/dev/null
  if [ -f "$SSH_CFG" ] && grep -qF "$BLOCK_A" "$SSH_CFG"; then
    awk -v a="$BLOCK_A" -v b="$BLOCK_B" -v repl="$content" '
      $0==a && !done { print repl; done=1; skip=1; next }
      skip && $0==b { skip=0; next }
      skip { next }
      { print }' "$SSH_CFG" > "$SSH_CFG.tmp-cn" && mv "$SSH_CFG.tmp-cn" "$SSH_CFG"
  else
    [ -f "$SSH_CFG" ] && cp "$SSH_CFG" "$SSH_CFG.bak-cn" 2>/dev/null
    printf '%s\n' "$content" >> "$SSH_CFG"
  fi
  chmod 600 "$SSH_CFG" 2>/dev/null
}

# ---------------------------------------------------------------- (1) proxy (opt-in, checked)
PROXY_SPEC=""
if [ "$PROXY_MODE" = 1 ]; then
  if [ "$PROXY_URL" = off ]; then
    gitq config --local --unset http.proxy 2>/dev/null
    gitq config --local --unset https.proxy 2>/dev/null
    gitq config --local --unset "http.https://github.com/.proxy" 2>/dev/null
    [ "$STATUS" = 1 ] || install_ssh_block ""
    say "proxy: removed (repo-local http/https.proxy cleared, ssh ProxyCommand removed)"
  else
    phostport="$(printf '%s' "$PROXY_URL" | sed 's#^[a-zA-Z0-9+]*://##; s#/.*$##')"
    phost="${phostport%%:*}"; pport="${phostport##*:}"
    case "$PROXY_URL" in socks5://*|socks://*) flag=-S ;; *) flag=-H ;; esac
    PROXY_SPEC="connect $flag ${phost}:${pport}"
    if timeout 5 bash -c "exec 3<>/dev/tcp/${phost}/${pport}" 2>/dev/null; then
      say "proxy: ${phost}:${pport} is listening"
      if [ "$STATUS" != 1 ]; then
        gitq config --local http.proxy "$PROXY_URL"
        gitq config --local https.proxy "$PROXY_URL"
        gitq config --local "http.https://github.com/.proxy" "$PROXY_URL"
        install_ssh_block "$PROXY_SPEC"
        say "proxy: written repo-locally (git) and into the ssh block (ProxyCommand $PROXY_SPEC)"
      fi
    else
      say "proxy: NOTHING WRITTEN - nothing is listening on ${phost}:${pport}"
      say "        start the proxy client first, or pass its real port: --proxy http://127.0.0.1:<port>"
      exit 1
    fi
  fi
fi

# ---------------------------------------------------------------- (2) baseline ssh + read fallbacks
if [ "$STATUS" != 1 ] && [ "$PROXY_SPEC" = "" ] && [ "$PROXY_MODE" = 0 ]; then
  install_ssh_block ""
  say "ssh config block: refreshed in $SSH_CFG"
fi

origin_url="$(gitq remote get-url origin 2>/dev/null || echo '')"
https_url=""
case "$origin_url" in
  git@github.com:*)       https_url="https://github.com/${origin_url#git@github.com:}" ;;
  ssh://git@github.com/*) https_url="https://github.com/${origin_url#ssh://git@github.com/}" ;;
  https://github.com/*)   https_url="$origin_url" ;;
esac

if [ -z "$https_url" ]; then
  say "read fallbacks: skipped (origin is not a github.com remote: ${origin_url:-none})"
elif [ "$STATUS" = 1 ]; then
  say "read fallbacks: github-https=$(gitq config --get remote.github-https.url || echo absent)"
  say "                cn-mirror   =$(gitq config --get remote.cn-mirror.url || echo absent)"
else
  gitq config remote.github-https.url "$https_url"
  gitq config remote.github-https.pushurl "$origin_url"
  gitq config remote.github-https.fetch "+refs/heads/*:refs/remotes/github-https/*" 
  gitq config remote.cn-mirror.url "$MIRROR/$https_url"
  gitq config remote.cn-mirror.pushurl "$origin_url"
  gitq config remote.cn-mirror.fetch "+refs/heads/*:refs/remotes/cn-mirror/*"
  say "read fallbacks: github-https (direct HTTPS), cn-mirror ($MIRROR) - fetch only, push pinned to SSH"
fi

# ---------------------------------------------------------------- (3) verify by measurement
say ""
say "== verify =="
t0=$(now_ms); ssh -o BatchMode=yes -o ConnectTimeout=10 -T git@github.com >/tmp/cn_s1.txt 2>&1
say "  ssh github.com:22     : $(ms_since "$t0") ms  $(tr -d '\r' < /tmp/cn_s1.txt | head -1 | cut -c1-30)"
t0=$(now_ms); ssh -o BatchMode=yes -o ConnectTimeout=10 -p 443 -T git@ssh.github.com >/tmp/cn_s2.txt 2>&1
say "  ssh ssh.github.com:443: $(ms_since "$t0") ms  $(tr -d '\r' < /tmp/cn_s2.txt | head -1 | cut -c1-30)"
curl -s -o /dev/null -m 15 -w "  https direct          : %{time_total}s total (connect %{time_connect}s)\n" https://github.com 2>/dev/null
# The effective ssh config, not what we hoped we wrote: ssh -G resolves first-match for real.
say "  ssh ProxyCommand in effect: $(ssh -G github.com 2>/dev/null | awk '/^proxycommand/{print $2" "$3" "$4; f=1} END{if(!f) print "none"}')"
if gitq config --get remote.github-https.url >/dev/null 2>&1; then
  t0=$(now_ms)
  if timeout 25 git -C "$REPO_ROOT" ls-remote github-https HEAD >/dev/null 2>&1; then say "  read via github-https : $(ms_since "$t0") ms (ok)"; else say "  read via github-https : FAILED right now"; fi
fi
if gitq config --get remote.cn-mirror.url >/dev/null 2>&1; then
  t0=$(now_ms)
  if timeout 25 git -C "$REPO_ROOT" ls-remote cn-mirror HEAD >/dev/null 2>&1; then
    say "  read via cn-mirror    : $(ms_since "$t0") ms (ok)"
  else
    say "  read via cn-mirror    : unreachable - retry MIRROR=https://ghproxy.net or https://ghfast.top"
  fi
fi
say "  repo-local http.proxy : $(gitq config --local --get http.proxy 2>/dev/null || echo none)"
say ""
say "Direct route down :  git fetch cn-mirror && git reset --hard cn-mirror/main"
say "Port 22 throttled :  git remote set-url origin git@github-443:<owner>/<repo>.git"
say "Writes stay on SSH.  No mirror ever sees a push, a key or a credential."
