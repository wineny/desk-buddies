#!/usr/bin/env python3
"""클로드봇 보드 자동 탐색 → board.json 갱신.

와이파이 망이 바뀌어(집↔회사) 보드 IP 가 달라졌을 때 실행한다:

    python3 find_board.py            # 탐색하고 board.json 갱신
    python3 find_board.py --rescan   # board.json 검증 건너뛰고 강제 재탐색

원리: 보드 MAC 은 어느 망에서나 같다. 서브넷을 ping 으로
훑어 ARP 테이블을 채운 뒤 그 MAC 을 가진 IP 를 찾는다. MAC 은 board.json 의 "mac" →
CLAUDEBOT_BOARD_MAC → 처음 보드(AA:BB:CC:DD:EE:FF) 순으로 쓴다. 그 MAC 이 망에 없으면
(다른 보드를 처음 연결할 때) --probe 면 ARP 에 보이는 기기마다 POST /usage {"agent":"claude"} 를 보내
보드처럼 답하는 곳({"ok":1,"agent":…,"char":…})을 찾고, 그 MAC 을 board.json 에 적는다.
{"agent":…} 단독 페이로드는 보드가 선택만 알려 주고 화면은 안 바꾸는 확인용이다. 현 펌웨어는 절전을
끄므로(PERFORMANCE) ping 에 응답한다 — 절전 펌웨어로 되돌리면 이 방법은
막히고 시리얼 부팅 로그(capture_boot)만 남는다.

찾은 IP 는 훅이 읽는 board.json (이 폴더)에 쓴다.
"""

from __future__ import annotations

import argparse
import ipaddress
import json
import re
import os
import subprocess
import sys
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

BOARD_MAC = "AA:BB:CC:DD:EE:FF"
HERE = Path(__file__).resolve().parent
BOARD_JSON_DIRS = [
    HERE,
]
MAX_SWEEP_HOSTS = 1024  # /22 까지만 훑는다(그보다 큰 망은 비현실적으로 오래 걸림)
PING_TIMEOUT_S = 1


def normalize_mac(mac: str) -> str:
    """arp -a 는 앞자리 0 을 떼고 출력한다(AA:BB:CC:DD:EE:FF → 그대로지만 0x0? 는 한 자리)."""
    return ":".join(f"{int(p, 16):02x}" for p in mac.split(":"))


def saved_mac() -> str | None:
    try:
        mac = json.loads((HERE / "board.json").read_text(encoding="utf-8")).get("mac")
    except (OSError, json.JSONDecodeError, AttributeError):
        return None
    return mac if isinstance(mac, str) and mac else None


def board_mac() -> str:
    return normalize_mac(saved_mac() or os.environ.get("CLAUDEBOT_BOARD_MAC", BOARD_MAC))


def arp_table() -> list[tuple[str, str]]:
    """(IP, MAC) 목록. 아직 답이 없는 칸(incomplete)은 뺀다."""
    out = subprocess.run(["arp", "-an"], capture_output=True, text=True, check=False).stdout
    rows = []
    for line in out.splitlines():
        m = re.search(r"\((\d+\.\d+\.\d+\.\d+)\) at ([0-9a-f:]+)", line, re.IGNORECASE)
        if m:
            rows.append((m.group(1), normalize_mac(m.group(2))))
    return rows


def arp_lookup() -> str | None:
    """ARP 테이블에서 보드 MAC 의 IP 를 찾는다."""
    mac = board_mac()
    return next((ip for ip, m in arp_table() if m == mac), None)


def looks_like_board(ip: str) -> bool:
    """보드의 확인용 응답({"ok":1,"agent":…,"char":…})이 오면 True."""
    req = urllib.request.Request(f"http://{ip}/usage", data=b'{"agent":"claude"}',
                                 headers={"Content-Type": "application/json"}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=1.5) as r:
            body = json.loads(r.read(200).decode("utf-8", "replace"))
    except (urllib.error.URLError, OSError, ValueError):
        return False
    return isinstance(body, dict) and body.get("ok") == 1 and "agent" in body and "char" in body


def probe_boards() -> list[tuple[str, str]]:
    """ARP 에 보이는 기기 중 보드처럼 답하는 것 (IP, MAC)."""
    rows = arp_table()
    with ThreadPoolExecutor(max_workers=50) as pool:
        hits = list(pool.map(lambda r: looks_like_board(r[0]), rows))
    return [r for r, ok in zip(rows, hits) if ok]


def local_network() -> ipaddress.IPv4Network | None:
    """기본 경로 인터페이스의 IPv4 네트워크(예: 10.3.50.0/23)를 알아낸다."""
    route = subprocess.run(
        ["route", "-n", "get", "default"], capture_output=True, text=True, check=False
    ).stdout
    m = re.search(r"interface: (\S+)", route)
    if not m:
        return None
    ifconfig = subprocess.run(
        ["ifconfig", m.group(1)], capture_output=True, text=True, check=False
    ).stdout
    m = re.search(r"inet (\d+\.\d+\.\d+\.\d+) netmask (0x[0-9a-f]+)", ifconfig)
    if not m:
        return None
    ip, mask_hex = m.group(1), m.group(2)
    mask = ipaddress.IPv4Address(int(mask_hex, 16))
    return ipaddress.IPv4Network(f"{ip}/{mask}", strict=False)


def ping_once(ip: str) -> None:
    subprocess.run(
        ["ping", "-c", "1", "-t", str(PING_TIMEOUT_S), ip],
        capture_output=True,
        check=False,
    )


def ping_sweep(net: ipaddress.IPv4Network) -> None:
    hosts = [str(h) for h in net.hosts()]
    if len(hosts) > MAX_SWEEP_HOSTS:
        print(f"망이 너무 크다({net}, {len(hosts)} 호스트) — 훑지 않음", file=sys.stderr)
        return
    print(f"{net} ({len(hosts)} 호스트) ping 스윕 중…")
    with ThreadPoolExecutor(max_workers=100) as pool:
        list(pool.map(ping_once, hosts))


def verify_board(ip: str) -> bool:
    """ip 를 한 번 ping 해 ARP 를 갱신하고, MAC 이 보드인지 확인한다."""
    ping_once(ip)
    return arp_lookup() == ip


def current_url() -> str | None:
    cfg = HERE / "board.json"
    if not cfg.is_file():
        return None
    try:
        url = json.loads(cfg.read_text(encoding="utf-8")).get("url")
        return url if isinstance(url, str) else None
    except (OSError, json.JSONDecodeError):
        return None


def write_board_json(ip: str, mac: str | None = None) -> None:
    mac = mac or board_mac()
    payload = json.dumps(
        {
            "url": f"http://{ip}/usage",
            "mac": mac,
            "_comment": "find_board.py 가 갱신. 망 옮기면 find_board.py 재실행.",
        },
        ensure_ascii=False,
        indent=2,
    )
    seen: set[Path] = set()
    for d in BOARD_JSON_DIRS:
        d = d.resolve() if d.exists() else d
        if d in seen or not d.is_dir():
            continue
        seen.add(d)
        (d / "board.json").write_text(payload + "\n", encoding="utf-8")
        print(f"기록: {d / 'board.json'}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rescan", action="store_true", help="board.json 검증 없이 강제 재탐색")
    ap.add_argument("--probe", action="store_true",
                    help="MAC 이 안 보이면 망의 기기마다 물어 보드를 찾는다(setup.py 가 켠다. 훅 자동 재탐색은 안 켬)")
    args = ap.parse_args()

    # 1) 지금 board.json 의 IP 가 여전히 보드인가
    if not args.rescan:
        url = current_url()
        m = re.search(r"//(\d+\.\d+\.\d+\.\d+)", url or "")
        if m and verify_board(m.group(1)):
            print(f"board.json 그대로 유효: {m.group(1)} (MAC 일치)")
            if not saved_mac():
                write_board_json(m.group(1))       # 옛 board.json 에 mac 을 채워 둔다
            return 0

    # 2) ARP 에 이미 있나 (최근 푸시가 성공했으면 남아 있다)
    ip = arp_lookup()

    # 3) 없으면 서브넷을 훑어 ARP 를 채운다
    if not ip:
        net = local_network()
        if net is None:
            print("네트워크 정보를 못 읽음(ifconfig/route)", file=sys.stderr)
            return 1
        ping_sweep(net)
        ip = arp_lookup()

    mac = None
    if not ip and args.probe:
        # 4) 그 MAC 이 없으면(다른 보드) 보드처럼 답하는 기기를 찾는다
        found = probe_boards()
        if len(found) > 1:
            print("보드가 여럿 보인다 — 쓸 보드의 MAC 을 board.json \"mac\" 에 적고 다시 돌릴 것:", file=sys.stderr)
            for fip, fmac in found:
                print(f"  {fip}  {fmac}", file=sys.stderr)
            return 1
        if found:
            ip, mac = found[0]

    if not ip:
        print(
            "보드를 못 찾음. 보드가 이 망의 WiFi 에 붙어 있는지 확인 —\n"
            "안 붙었으면 보드 화면의 WIFI_CONFIG_MODE 상태에서 핫스팟\n"
            "Xiaozhi-XXXX → http://192.168.4.1 로 이 망을 등록할 것.",
            file=sys.stderr,
        )
        return 1

    print(f"보드 발견: {ip} (MAC {mac or board_mac()})")
    write_board_json(ip, mac)
    return 0


if __name__ == "__main__":
    sys.exit(main())
