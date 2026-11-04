"""MIT. Separate updater UI: the game is never terminated to apply an update."""
import argparse
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
import time
import tkinter as tk
from tkinter import messagebox, ttk
from update_client import check, download
from version_info import version_tuple

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--target', type=Path, required=True)
    parser.add_argument('--current', required=True)
    parser.add_argument('--automatic', action='store_true')
    parser.add_argument('--smoke-test', action='store_true')
    args = parser.parse_args()
    version_tuple(args.current)
    target = args.target.resolve()
    cache = Path(os.environ.get('LOCALAPPDATA', Path.home())) / 'PowerfulMoon/updates'
    cache.mkdir(parents=True, exist_ok=True)
    stamp = cache / 'last-check.json'
    if args.automatic:
        try:
            if time.time() - json.loads(stamp.read_text())['checked'] < 86400:
                return
        except (OSError, ValueError, KeyError):
            pass
    root = tk.Tk()
    root.title('PowerfulMoon Update')
    root.geometry('620x330')
    root.resizable(False, False)
    try:
        root.iconbitmap(str(Path(getattr(sys, '_MEIPASS', Path(__file__).parent)) / 'icon.ico'))
    except tk.TclError:
        pass
    root.option_add('*Font', '{Segoe UI} 10')
    box = ttk.Frame(root, padding=26)
    box.pack(fill='both', expand=True)
    ttk.Label(box, text='PowerfulMoon Update', font=('Segoe UI', 21, 'bold')).pack(anchor='w')
    ttk.Label(box, text='Installed: ' + args.current).pack(anchor='w', pady=8)
    ttk.Label(box, text='Updates come from approved PowerfulMoon releases. Your saves, mods and controller settings stay in this folder.', wraplength=555).pack(anchor='w')
    status = tk.StringVar(value='Checking for updates...')
    ttk.Label(box, textvariable=status, wraplength=555).pack(anchor='w', pady=14)
    bar = ttk.Progressbar(box, mode='indeterminate')
    bar.pack(fill='x')
    events = queue.Queue()
    state = {'busy': True, 'update': None, 'installer': None}
    if args.automatic:
        root.withdraw()
    def check_worker():
        try:
            update = check(args.current)
            stamp.write_text(json.dumps({'checked': time.time()}))
            events.put(('checked', update))
        except Exception as error:
            events.put(('error', str(error)))
    def download_worker():
        try:
            result = download(state['update'], cache, lambda n, total: events.put(('progress', 100 * n / total)))
            events.put(('downloaded', result))
        except Exception as error:
            events.put(('error', str(error)))
    def action():
        if state['installer']:
            if not messagebox.askokcancel('Install update', 'Save your progress and close PowerfulMoon first.\n\nInstall into:\n' + str(target) + '\n\nThe installer will refuse to replace a running game.'):
                return
            try:
                subprocess.Popen([str(state['installer']), '--upgrade-folder', str(target)], creationflags=0x08000000)
                # Exit before Setup replaces this EXE: Windows locks a running helper.
                root.destroy()
            except OSError as error:
                messagebox.showerror('Could not open installer', str(error))
            return
        else:
            worker = download_worker
            status.set('Downloading and verifying installer...')
        state['busy'] = True
        button.configure(state='disabled')
        bar.start(12)
        threading.Thread(target=worker, daemon=True).start()
    button = ttk.Button(box, text='Download update', command=action, state='disabled')
    button.pack(anchor='e', pady=14)
    def poll():
        try:
            while True:
                kind, value = events.get_nowait()
                if kind == 'progress':
                    status.set('Downloading: %.0f%%' % value)
                    continue
                state['busy'] = False
                bar.stop()
                if kind == 'checked':
                    if value is None:
                        if args.automatic:
                            root.destroy(); return
                        status.set('You have the latest approved release.')
                    else:
                        root.deiconify()
                        state['update'] = value
                        status.set('Available: ' + value.version + '. Download when ready.')
                        button.configure(state='normal')
                elif kind == 'downloaded':
                    state['installer'] = value
                    status.set('Download verified. Save and close the game, then install.')
                    button.configure(text='Install update', state='normal')
                else:
                    if args.automatic and state['update'] is None:
                        root.destroy(); return
                    status.set('Update failed: ' + value)
                    if state['update']:
                        button.configure(state='normal')
        except queue.Empty:
            pass
        root.after(100, poll)
    root.protocol('WM_DELETE_WINDOW', lambda: None if state['busy'] else root.destroy())
    root.after(100, poll)
    if args.smoke_test:
        root.after(700, root.destroy)
    else:
        bar.start(12)
        threading.Thread(target=check_worker, daemon=True).start()
    root.mainloop()

if __name__ == '__main__':
    main()
