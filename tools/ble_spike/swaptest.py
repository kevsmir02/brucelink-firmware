import asyncio,sys,time,os,termios,select,threading,re
from bleak import BleakClient, BleakScanner
CLI="d555ed97-bf2a-4f46-b3eb-d1fcdd7325e9"; EVT="d555ed98-bf2a-4f46-b3eb-d1fcdd7325e9"
VERB=sys.argv[1] if len(sys.argv)>1 else "blespam fastpair_regular 10"
T0=time.time(); usb=[]; stop=threading.Event()
def ts(): return f"+{time.time()-T0:6.2f}s"
def usb_reader():
    fd=None; buf=b''
    while not stop.is_set():
        if fd is None:
            try:
                fd=os.open('/dev/ttyACM0',os.O_RDWR|os.O_NOCTTY|os.O_NONBLOCK)
                a=termios.tcgetattr(fd);a[0]=0;a[1]=0;a[3]=0
                a[2]=termios.CS8|termios.CREAD|termios.CLOCAL;a[4]=a[5]=termios.B115200
                cc=list(a[6]);cc[termios.VMIN]=0;cc[termios.VTIME]=0;a[6]=cc
                termios.tcsetattr(fd,termios.TCSANOW,a);termios.tcflush(fd,termios.TCIOFLUSH)
            except Exception: time.sleep(0.3); continue
        try:
            r,_,_=select.select([fd],[],[],0.1)
            if r:
                c=os.read(fd,8192)
                if c:
                    buf+=c
                    while b'\n' in buf:
                        l,_,buf=buf.partition(b'\n')
                        s=l.decode('utf-8','replace').rstrip()
                        if s and 'i2c_master_transmit' not in s:
                            usb.append((time.time()-T0,s)); print(f"{ts()}  USB  {s}",flush=True)
        except OSError:
            try: os.close(fd)
            except Exception: pass
            fd=None; time.sleep(0.4)
threading.Thread(target=usb_reader,daemon=True).start()
async def main():
    dev=await BleakScanner.find_device_by_name("Bruc",timeout=20.0)
    if not dev: print("FAIL: not advertising"); stop.set(); return 2
    disc=asyncio.Event(); t_disc=[None]
    def on_disc(_c):
        t_disc[0]=time.time()-T0; print(f"{ts()}  *** BLE LINK DROPPED ***",flush=True); disc.set()
    cli_txt=[]
    async with BleakClient(dev,disconnected_callback=on_disc) as c:
        evt=bytearray()
        def on_evt(_s,d):
            evt.extend(d)
            while b'\n' in evt:
                l,_,r=bytes(evt).partition(b'\n'); evt.clear(); evt.extend(r)
                if l.strip(): print(f"{ts()}  EVENT  {l.decode('utf-8','replace').strip()}",flush=True)
        def on_cli(_s,d):
            t=bytes(d).decode('utf-8','replace'); cli_txt.append(t)
            print(f"{ts()}  CLI    {t!r}",flush=True)
        await c.start_notify(EVT,on_evt); await c.start_notify(CLI,on_cli)
        await asyncio.sleep(1)
        print(f"{ts()}  >>> SEND '{VERB}'",flush=True)
        await c.write_gatt_char(CLI,(VERB+"\n").encode(),response=True)
        end=time.time()+45
        while time.time()<end and not disc.is_set(): await asyncio.sleep(0.2)
        if not disc.is_set(): print(f"{ts()}  (link never dropped)",flush=True)
    # measure recovery
    print(f"{ts()}  --- polling for re-advertisement ---",flush=True)
    t_back=None
    deadline=time.time()+90
    while time.time()<deadline:
        d2=await BleakScanner.find_device_by_name("Bruc",timeout=6.0)
        if d2:
            t_back=time.time()-T0
            print(f"{ts()}  *** 'Bruc' ADVERTISING AGAIN ***",flush=True)
            try:
                async with BleakClient(d2) as c2:
                    buf=bytearray(); ev=asyncio.Event()
                    def on2(_s,dd):
                        buf.extend(dd)
                        if 0x04 in buf: ev.set()
                    await c2.start_notify(CLI,on2)
                    await c2.write_gatt_char(CLI,b"free\n",response=True)
                    await asyncio.wait_for(ev.wait(),10)
                    print(f"{ts()}  POST-SWAP free: {bytes(buf).split(b'\\x04')[0].decode('utf-8','replace').strip()}",flush=True)
            except Exception as e: print(f"{ts()}  reconnect/exec failed: {e}",flush=True)
            break
        await asyncio.sleep(1)
    await asyncio.sleep(1); stop.set(); time.sleep(0.5)
    blob="\n".join(s for _,s in usb)
    print("\n================ VERDICT ================")
    print(f"verb                     : {VERB}")
    print(f"suspend notice on CLI    : {'YES' if any('suspended' in t for t in cli_txt) else 'NO'}")
    print(f"BLE link dropped at      : {('+%.2fs'%t_disc[0]) if t_disc[0] else 'never dropped'}")
    print(f"re-advertised at         : {('+%.2fs'%t_back) if t_back else 'NOT within 90s'}")
    if t_disc[0] and t_back: print(f"control-link outage      : {t_back-t_disc[0]:.1f}s")
    print(f"AP-restore fallback fired: {'YES (investigate)' if 'Restoring WiFi AP' in blob else 'no'}")
    print(f"crashed                  : {'YES' if re.search(r'assert failed|Backtrace:|Guru Meditation',blob) else 'no'}")
    print("=========================================")
    return 0
sys.exit(asyncio.run(main()))
