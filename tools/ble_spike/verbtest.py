import asyncio,sys,time,os,termios,select,threading,re
from bleak import BleakClient, BleakScanner
CLI="d555ed97-bf2a-4f46-b3eb-d1fcdd7325e9"; EVT="d555ed98-bf2a-4f46-b3eb-d1fcdd7325e9"
VERB=sys.argv[1]; DUR=float(sys.argv[2]) if len(sys.argv)>2 else 90.0
T0=time.time(); usb_lines=[]; stop=threading.Event()
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
                            usb_lines.append((time.time()-T0,s)); print(f"{ts()}  USB  {s}",flush=True)
        except OSError:
            try: os.close(fd)
            except Exception: pass
            fd=None; time.sleep(0.4)
threading.Thread(target=usb_reader,daemon=True).start()
async def main():
    print(f"===== TESTING VERB: {VERB}   (window {DUR:.0f}s) =====",flush=True)
    dev=await BleakScanner.find_device_by_name("Bruc",timeout=25.0)
    if not dev: print("FAIL: 'Bruc' not advertising"); stop.set(); return 2
    disc=asyncio.Event()
    def on_disc(_c): print(f"{ts()}  *** BLE DISCONNECTED ***",flush=True); disc.set()
    try:
        async with BleakClient(dev,disconnected_callback=on_disc) as c:
            evt=bytearray(); replied=[]
            def on_evt(_s,d):
                evt.extend(d)
                while b'\n' in evt:
                    l,_,r=bytes(evt).partition(b'\n'); evt.clear(); evt.extend(r)
                    if l.strip(): print(f"{ts()}  EVENT  {l.decode('utf-8','replace').strip()}",flush=True)
            def on_cli(_s,d):
                replied.append(1); print(f"{ts()}  CLI    {bytes(d).decode('utf-8','replace')!r}",flush=True)
            await c.start_notify(EVT,on_evt); await c.start_notify(CLI,on_cli)
            await asyncio.sleep(1)
            print(f"{ts()}  >>> SEND '{VERB}'",flush=True)
            await c.write_gatt_char(CLI,(VERB+"\n").encode(),response=True)
            end=time.time()+DUR
            while time.time()<end and not disc.is_set(): await asyncio.sleep(0.2)
    except Exception as e:
        print(f"{ts()}  client error: {e}",flush=True)
    await asyncio.sleep(2); stop.set(); time.sleep(0.5)
    blob="\n".join(s for _,s in usb_lines)
    crashed = bool(re.search(r'assert failed|Backtrace:|Guru Meditation|Rebooting\.\.\.|rst:0x', blob))
    bt = next((s for _,s in usb_lines if s.startswith('Backtrace:')), None)
    assertion = next((s for _,s in usb_lines if 'assert failed' in s or 'Guru' in s), None)
    print("\n================ VERDICT ================")
    print(f"verb           : {VERB}")
    print(f"CRASHED        : {'YES' if crashed else 'no crash within %.0fs'%DUR}")
    if assertion: print(f"assertion      : {assertion}")
    if bt: print(f"backtrace      : {bt}")
    print("=========================================")
    return 0
sys.exit(asyncio.run(main()))
