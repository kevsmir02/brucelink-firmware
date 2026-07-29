import os,time,termios,select,sys
DUR=float(sys.argv[1]); t0=time.time()
def openp():
    fd=os.open('/dev/ttyACM0',os.O_RDWR|os.O_NOCTTY|os.O_NONBLOCK)
    a=termios.tcgetattr(fd);a[0]=0;a[1]=0;a[3]=0
    a[2]=termios.CS8|termios.CREAD|termios.CLOCAL;a[4]=a[5]=termios.B115200
    cc=list(a[6]);cc[termios.VMIN]=0;cc[termios.VTIME]=0;a[6]=cc
    termios.tcsetattr(fd,termios.TCSANOW,a);termios.tcflush(fd,termios.TCIOFLUSH)
    return fd
fd=None; buf=b''
while time.time()-t0<DUR:
    if fd is None:
        try:
            fd=openp(); print(f"+{time.time()-t0:6.2f}s  [USB PORT OPENED]",flush=True)
        except Exception as e:
            time.sleep(0.3); continue
    try:
        r,_,_=select.select([fd],[],[],0.1)
        if r:
            c=os.read(fd,8192)
            if c:
                buf+=c
                while b'\n' in buf:
                    l,_,rest=buf.partition(b'\n'); buf=rest
                    s=l.decode('utf-8','replace').rstrip()
                    if s and 'i2c_master_transmit' not in s:
                        print(f"+{time.time()-t0:6.2f}s  USB  {s}",flush=True)
    except (OSError,BlockingIOError) as e:
        print(f"+{time.time()-t0:6.2f}s  [USB PORT LOST: {e}]",flush=True)
        try: os.close(fd)
        except Exception: pass
        fd=None; time.sleep(0.5)
print("[usb watcher done]",flush=True)
