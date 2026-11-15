"""Opt-in Linux Steam shortcut creation. No game content or third-party modules.

Steam must be closed. Existing entries/unknown fields are preserved, writes are
atomic, and undo refuses to overwrite a file changed since this operation.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import struct
import sys
import tempfile
import uuid
import zlib

NAME = 'Super Odyssey PC Recomp'
MARKER = 'SuperOdysseyDataRoot'
LIMIT = 16 * 1024 * 1024


def decode(data):
    if len(data) > LIMIT:
        raise ValueError('Steam shortcuts file is too large')
    at = 0
    count = 0
    def string():
        nonlocal at
        end = data.find(b'\0', at)
        if end < 0:
            raise ValueError('Unterminated Steam shortcut string')
        value = data[at:end].decode('utf-8', 'surrogateescape')
        at = end + 1
        return value
    def nodes(depth):
        nonlocal at, count
        if depth > 32:
            raise ValueError('Steam shortcuts nesting is too deep')
        result = []
        while at < len(data):
            kind = data[at]
            at += 1
            if kind == 8:
                return result
            count += 1
            if count > 100000:
                raise ValueError('Too many Steam shortcut fields; nothing changed')
            key = string()
            if kind == 0:
                value = nodes(depth + 1)
            elif kind == 1:
                value = string()
            elif kind in (2, 3, 4, 6, 7, 10):
                size = 8 if kind in (7, 10) else 4
                value = data[at:at + size]
                if len(value) != size:
                    raise ValueError('Truncated Steam shortcut value')
                at += size
            else:
                raise ValueError(f'Unsupported Steam shortcut field type {kind}; nothing changed')
            result.append((kind, key, value))
        raise ValueError('Missing Steam shortcuts object terminator')
    # The top-level document contains one named object; some clients also write
    # an outer END marker. Preserve that choice rather than normalizing it.
    if not data.startswith(b'\x00shortcuts\0'):
        raise ValueError('Unrecognized Steam shortcuts file; nothing changed')
    at = len(b'\x00shortcuts\0')
    entries = nodes(0)
    ending = data[at:]
    if ending not in (b'', b'\x08'):
        raise ValueError('Unexpected bytes after Steam shortcuts object')
    return entries, ending


def encode(entries, ending=b'\x08'):
    def text(value):
        if '\0' in value:
            raise ValueError('NUL in Steam shortcut field')
        return value.encode('utf-8', 'surrogateescape') + b'\0'
    def nodes(values):
        chunks = []
        for kind, key, value in values:
            chunks.extend((bytes([kind]), text(key)))
            chunks.append(nodes(value) if kind == 0 else text(value) if kind == 1 else value)
        return b''.join(chunks) + b'\x08'
    return b'\x00shortcuts\0' + nodes(entries) + ending


def steam_running():
    if not sys.platform.startswith('linux'):
        return False
    for proc in Path('/proc').iterdir():
        if not proc.name.isdecimal():
            continue
        try:
            if proc.stat().st_uid == os.getuid() and (proc / 'comm').read_text().strip() in ('steam', 'steam.sh'):
                return True
        except (OSError, UnicodeError):
            pass
    return False


def steam_root():
    home = Path.home()
    for path in (home / '.steam/root', home / '.local/share/Steam',
                 home / '.var/app/com.valvesoftware.Steam/.local/share/Steam'):
        if (path / 'userdata').is_dir():
            return path.resolve()
    raise ValueError('Steam was not found. Install Steam and sign in once first.')


def login_accounts(text):
    # Keep quoted strings distinct from object braces: a persona name can
    # legally contain braces or escaped quotes.
    token = re.compile(r'\s*(?:(//[^\n]*)|([{}])|"((?:\\.|[^"\\])*)")')
    tokens, at = [], 0
    while at < len(text):
        match = token.match(text, at)
        if not match:
            if text[at:].strip():
                raise ValueError('Invalid Steam login metadata')
            break
        at = match.end()
        if match[1] is not None:
            continue
        tokens.append((match[2], None) if match[2] else ('text', re.sub(r'\\(["\\])', r'\1', match[3])))
    at = 0
    def object_(depth=0):
        nonlocal at
        if depth > 16:
            raise ValueError('Steam login metadata nesting is too deep')
        result = {}
        while at < len(tokens):
            kind, key = tokens[at]
            at += 1
            if kind == '}':
                return result
            if kind != 'text' or at == len(tokens):
                raise ValueError('Invalid Steam login metadata')
            kind, value = tokens[at]
            at += 1
            if key in result:
                raise ValueError('Duplicate Steam login metadata')
            if kind == '{':
                result[key] = object_(depth + 1)
            elif kind == 'text':
                result[key] = value
            else:
                raise ValueError('Invalid Steam login metadata')
        if depth:
            raise ValueError('Unterminated Steam login metadata')
        return result
    result = object_()
    if at != len(tokens):
        raise ValueError('Unexpected Steam login metadata')
    users = result.get('users', {})
    if not isinstance(users, dict):
        raise ValueError('Invalid Steam users metadata')
    return users


def account(root, selected=None):
    users = sorted(p.name for p in (root / 'userdata').iterdir() if p.is_dir() and p.name.isdecimal())
    if selected:
        if selected not in users:
            raise ValueError('That Steam account was not found')
        return selected
    if len(users) == 1:
        return users[0]
    login = root / 'config/loginusers.vdf'
    if login.is_file() and login.stat().st_size < 1024 * 1024:
        recent = []
        try:
            metadata = login_accounts(login.read_text(errors='replace'))
        except ValueError:
            metadata = {}
        for identifier, body in metadata.items():
            if identifier.isdecimal() and len(identifier) == 17 and isinstance(body, dict) and body.get('MostRecent') == '1':
                user = str(int(identifier) - 76561197960265728)
                if user in users:
                    recent.append(user)
        if len(recent) == 1:
            return recent[0]
    raise ValueError('No unambiguous Steam account. Sign in to the desired account, quit Steam and retry.')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def atomic_write(path, data, mode=0o600):
    fd, temporary = tempfile.mkstemp(prefix='.superodyssey-', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, mode)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def add(root, appimage, data_root, receipt, selected=None):
    if steam_running():
        raise ValueError('Quit Steam first, then press Add to Steam again.')
    root, appimage, data_root = root.resolve(), appimage.resolve(), data_root.resolve()
    if not appimage.is_file() or not data_root.is_dir():
        raise ValueError('The AppImage or installed data folder was not found')
    if any(c in str(appimage) for c in '\n\r\0'):
        raise ValueError('The AppImage path contains an unsupported control character')
    config = root / 'userdata' / account(root, selected) / 'config'
    config.mkdir(exist_ok=True)
    path = config / 'shortcuts.vdf'
    if path.is_symlink():
        raise ValueError('Steam shortcuts is a symbolic link; nothing changed')
    if path.exists() and path.stat().st_size > LIMIT:
        raise ValueError('Steam shortcuts file is too large')
    before = path.read_bytes() if path.exists() else None
    entries, ending = decode(before) if before is not None else ([], b'\x08')
    def quote_path(value):
        return '"' + str(value).replace('\\', '\\\\').replace('"', '\\"').replace('$', '\\$').replace('`', '\\`') + '"'
    exe = quote_path(appimage)
    launch = 'env ' + shlex.quote('ODYSSEY_DATA_ROOT=' + str(data_root)) + ' %command%'
    values = {'AppName': NAME, 'Exe': exe, 'StartDir': quote_path(appimage.parent),
              'LaunchOptions': launch, MARKER: str(data_root)}
    matches = []
    for index, (kind, key, fields) in enumerate(entries):
        if kind != 0:
            continue
        strings = {k.casefold(): v for t, k, v in fields if t == 1}
        if strings.get(MARKER.casefold()) == str(data_root) or (
                strings.get('appname') == NAME and strings.get('exe') == exe):
            matches.append(index)
    if len(matches) > 1:
        raise ValueError('Several matching Steam shortcuts exist. Remove duplicates in Steam first.')
    if matches:
        index = matches[0]
        kind, key, fields = entries[index]
        keys = [k.casefold() for _, k, _ in fields if k.casefold() in {v.casefold() for v in values}]
        if len(keys) != len(set(keys)):
            raise ValueError('Duplicate fields in matching Steam shortcut; nothing changed.')
        existing = {k.casefold(): (t, v) for t, k, v in fields}
        if all(existing.get(k.casefold()) == (1, v) for k, v in values.items()):
            return 'Already added to Steam.'
        # Preserve the original appid, controller layout, playtime, tags, icons
        # and unknown client fields when the AppImage moves or options change.
        replacements = {k.casefold(): (1, k, v) for k, v in values.items()}
        updated = [replacements.pop(k.casefold(), (t, k, v)) for t, k, v in fields]
        entries[index] = (kind, key, updated + list(replacements.values()))
    else:
        used = {key for _, key, _ in entries}
        index = next(i for i in range(len(entries) + 1) if str(i) not in used)
        appid = zlib.crc32((exe + NAME).encode()) | 0x80000000
        fields = [(2, 'appid', struct.pack('<I', appid))] + [(1, k, v) for k, v in values.items()]
        fields += [(1, 'icon', ''), (1, 'ShortcutPath', '')]
        fields += [(2, k, struct.pack('<I', v)) for k, v in
                   (('IsHidden', 0), ('AllowDesktopConfig', 1), ('AllowOverlay', 1), ('OpenVR', 0), ('LastPlayTime', 0))]
        fields.append((0, 'tags', []))
        entries.append((0, str(index), fields))
    after = encode(entries, ending)
    if len(after) > LIMIT:
        raise ValueError('Updated Steam shortcuts would exceed the size limit')
    backup = config / ('shortcuts.superodyssey-' + uuid.uuid4().hex + '.bak') if before is not None else None
    if backup:
        with backup.open('xb') as stream:
            stream.write(before)
            stream.flush()
            os.fsync(stream.fileno())
    transaction = {'path': str(path), 'backup': str(backup) if backup else None,
                   'before': digest(before) if before is not None else None, 'after': digest(after)}
    atomic_write(receipt, json.dumps(transaction, indent=2).encode())
    if steam_running() or (path.read_bytes() if path.exists() else None) != before:
        raise ValueError('Steam or its shortcuts changed while preparing the shortcut; retry with Steam closed.')
    mode = path.stat().st_mode & 0o777 if path.exists() else 0o600
    atomic_write(path, after, mode)
    return 'Added to Steam. Start Steam to see Super Odyssey PC Recomp.'


def undo(receipt):
    if steam_running():
        raise ValueError('Quit Steam before undoing the shortcut.')
    transaction = json.loads(receipt.read_text())
    path = Path(transaction['path'])
    if path.name != 'shortcuts.vdf' or path.is_symlink() or digest(path.read_bytes()) != transaction['after']:
        raise ValueError('Steam shortcuts changed since this operation; nothing was overwritten.')
    if transaction['backup']:
        backup = Path(transaction['backup'])
        if backup.parent != path.parent or backup.is_symlink() or not backup.name.startswith('shortcuts.superodyssey-'):
            raise ValueError('Invalid shortcut backup path')
        before = backup.read_bytes()
        if digest(before) != transaction['before']:
            raise ValueError('Steam shortcut backup checksum differs; nothing changed.')
        atomic_write(path, before, path.stat().st_mode & 0o777)
    else:
        path.unlink()
    receipt.unlink()
    return 'Steam shortcut change undone.'


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--appimage', type=Path)
    ap.add_argument('--data-root', type=Path)
    ap.add_argument('--receipt', type=Path, required=True)
    ap.add_argument('--root', type=Path)
    ap.add_argument('--account')
    ap.add_argument('--undo', action='store_true')
    args = ap.parse_args(argv)
    try:
        if args.undo:
            message = undo(args.receipt)
        else:
            if not args.appimage or not args.data_root:
                ap.error('--appimage and --data-root are required when adding a shortcut')
            message = add(args.root or steam_root(), args.appimage, args.data_root, args.receipt, args.account)
        print(message)
        return 0
    except (OSError, ValueError) as error:
        print(str(error))
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
