"""MIT. PowerfulMoon Windows full installer; no external runtime required."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import threading
import queue
import tkinter as tk
from tkinter import ttk,filedialog,messagebox
from installer_core import install

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--target',type=Path,help='Install to this folder without the wizard')
    parser.add_argument('--upgrade-folder',type=Path,help='Preselect an existing folder in the upgrade wizard')
    parser.add_argument('--archive',type=Path,help='Use a local PowerfulMoon package (developer mode)')
    parser.add_argument('--smoke-test',action='store_true')
    args=parser.parse_args()
    resources=Path(getattr(sys,'_MEIPASS',Path(__file__).parent))
    archive=args.archive or resources/'PowerfulMoon-windows-x64.zip'
    def do_install(target,progress):
        # Never terminate a running game or risk its unsaved progress.
        literal=str(target.resolve()).replace("'","''")
        script="$p='"+literal+"'; if(Get-Process PowerfulMoon,PowerMoonRecomp -ErrorAction SilentlyContinue | Where-Object { $_.Path -and (Split-Path $_.Path -Parent) -eq $p }) { exit 9 }"
        if subprocess.run(['powershell.exe','-NoProfile','-Command',script],creationflags=0x08000000).returncode==9:
            raise RuntimeError('Save your progress and close the game before installing.')
        exe=install(archive,target,progress)
        def quote(value):return "'"+str(value).replace("'","''")+"'"
        link=Path(os.environ['APPDATA'])/'Microsoft/Windows/Start Menu/Programs/PowerfulMoon.lnk'
        shortcut="$w=New-Object -ComObject WScript.Shell; $s=$w.CreateShortcut("+quote(link)+"); $s.TargetPath="+quote(exe)+"; $s.WorkingDirectory="+quote(exe.parent)+"; $s.Save()"
        subprocess.run(['powershell.exe','-NoProfile','-Command',shortcut],creationflags=0x08000000,check=True)
        return exe
    if args.target:
        do_install(args.target,lambda _:None)
        return
    root=tk.Tk();root.title('PowerfulMoon Setup');root.geometry('700x520');root.resizable(False,False)
    try:root.iconbitmap(str(resources/'icon.ico'))
    except tk.TclError:pass
    root.option_add('*Font','{Segoe UI} 10')
    box=ttk.Frame(root,padding=26);box.pack(fill='both',expand=True)
    ttk.Label(box,text='PowerfulMoon',font=('Segoe UI',23,'bold')).pack(anchor='w')
    ttk.Label(box,text='Customized fork of Power Moon Recomp by zzzarathustra-dev',wraplength=590).pack(anchor='w',pady=(3,18))
    ttk.Label(box,text='Install or upgrade folder',font=('Segoe UI',10,'bold')).pack(anchor='w')
    folder=tk.StringVar(value=str(args.upgrade_folder or Path(os.environ['LOCALAPPDATA'])/'Programs/PowerfulMoon'))
    row=ttk.Frame(box);row.pack(fill='x',pady=7)
    ttk.Entry(row,textvariable=folder).pack(side='left',fill='x',expand=True)
    ttk.Button(row,text='Browse...',command=lambda:choose()).pack(side='left',padx=(8,0))
    ttk.Label(box,text='Upgrading from Power Moon? Choose its existing game folder to keep your saves and controller settings. The game-code cache will rebuild for the renamed app.',wraplength=590).pack(anchor='w',pady=(0,10))
    ttk.Label(box,text='Includes VRR interpolation fix and Switch 2 Pro Controller bridge. No game files, keys or Nintendo assets are included.',wraplength=590).pack(anchor='w',pady=(0,14))
    status=tk.StringVar(value='Ready. Close the game before installing.')
    ttk.Label(box,textvariable=status,wraplength=590).pack(anchor='w')
    bar=ttk.Progressbar(box,mode='indeterminate');bar.pack(fill='x',pady=10)
    events=queue.Queue();busy=False
    def choose():
        selected=filedialog.askdirectory(title='Choose install or existing Power Moon folder')
        if selected:folder.set(selected)
    def work(target):
        try:events.put(('done',do_install(target,lambda text:events.put(('status',text)))))
        except Exception as error:events.put(('error',str(error)))
    def start():
        nonlocal busy
        busy=True;button.configure(state='disabled');bar.start(12)
        threading.Thread(target=work,args=(Path(folder.get()),),daemon=True).start()
    def close():
        if not busy:root.destroy()
    def poll():
        nonlocal busy
        try:
            while True:
                kind,value=events.get_nowait()
                if kind=='status':status.set(value)
                else:
                    busy=False;bar.stop();button.configure(state='normal')
                    if kind=='error':messagebox.showerror('Installation failed',value);status.set('Installation failed; existing program files were restored.')
                    else:
                        status.set('Installed successfully. Your existing user files were retained.')
                        if messagebox.askyesno('PowerfulMoon installed','Open PowerfulMoon now?\n\nThe controller bridge may ask for Windows approval for its signed driver on first use.'):
                            subprocess.Popen([str(value)],cwd=str(value.parent));root.destroy();return
        except queue.Empty:pass
        root.after(100,poll)
    button=ttk.Button(box,text='Install / Upgrade',command=start);button.pack(anchor='e',pady=(5,0))
    root.protocol('WM_DELETE_WINDOW',close);root.after(100,poll)
    if args.smoke_test:root.after(800,root.destroy)
    root.mainloop()
if __name__=='__main__':main()
