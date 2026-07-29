"""Exercise the WebUI HTTP routes over the device AP.

usage: http_routes.py [base_url]        default http://172.0.0.1

Logs in ONCE and reuses the cookie for every request. That is not laziness:
POST /login rewrites the whole config to flash on every call and has been seen to
abort the device under load (ISSUE-18), so repeated logins are themselves a
hazard. If a route needs auth and the cookie has expired, re-run the script
rather than adding a second login.

Reports status, byte count and content-type per route. A route that 404s is a
finding (the route is not registered in this build); a route that 401s means the
cookie was rejected; a route that hangs is the interesting case and is bounded by
the timeout rather than left to stall.
"""
import sys
import json
import urllib.request
import urllib.parse
import urllib.error

BASE = sys.argv[1] if len(sys.argv) > 1 else "http://172.0.0.1"
USER, PWD = "admin", "bruce"
TIMEOUT = 12

cookie = None


def req(method, path, data=None, ctype=None, timeout=TIMEOUT):
    url = BASE + path
    r = urllib.request.Request(url, data=data, method=method)
    if cookie:
        r.add_header("Cookie", cookie)
    if ctype:
        r.add_header("Content-Type", ctype)
    try:
        with urllib.request.urlopen(r, timeout=timeout) as resp:
            body = resp.read()
            return resp.status, dict(resp.headers), body
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()
    except Exception as e:
        return None, {}, f"{type(e).__name__}: {e}".encode()


def show(label, status, hdrs, body, preview=90):
    ct = hdrs.get("Content-Type", "-")
    n = len(body)
    head = body[:preview].decode("utf-8", "replace").replace("\n", " ")
    print(f"  {label:<34} {str(status):<6} {n:>7} B  {ct:<26} {head}")


def main():
    global cookie
    print(f"=== {BASE} ===\n")

    # unauthenticated probe first — establishes that auth is actually enforced
    print("unauthenticated:")
    for p in ["/", "/systeminfo"]:
        s, h, b = req("GET", p)
        show(p, s, h, b)

    print("\nlogin (ONCE — see ISSUE-18):")
    payload = json.dumps({"user": USER, "pwd": PWD}).encode()
    s, h, b = req("POST", "/login", payload, "application/json")
    show("POST /login", s, h, b)
    sc = h.get("Set-Cookie")
    if sc:
        cookie = sc.split(";")[0]
        print(f"  cookie -> {cookie}")
    else:
        print("  no Set-Cookie returned; continuing unauthenticated")

    print("\nauthenticated GETs:")
    for p in [
        "/systeminfo",
        "/getscreen",
        "/listfiles?folder=/&fs=LittleFS",
        "/file?name=/bruce.conf&action=download&fs=LittleFS",
    ]:
        s, h, b = req("GET", p)
        show(p, s, h, b)

    print("\ncommand route:")
    s, h, b = req("POST", "/cm?cmnd=" + urllib.parse.quote("uptime"))
    show("POST /cm cmnd=uptime", s, h, b)
    s, h, b = req("GET", "/cm?cmnd=uptime")
    show("GET /cm (should 404)", s, h, b)

    print("\ndone. Routes returning 404 are not registered in this build.")


if __name__ == "__main__":
    main()
