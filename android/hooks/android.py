import os
import shlex
import subprocess


def _exec(*cmd):
    print("+ " + shlex.join(cmd))
    return subprocess.check_output(cmd, text=True)


def get_session_info(session_id):
    return {"backend": "opengles", "system": "Android"}


def get_sessions():
    output = _exec("adb", "devices", "-l").rstrip()
    return [line.split(maxsplit=1) for line in output.splitlines()[1:]]


def sync_file(session_id, ifile, ofile):
    dst_dir = _exec("adb", "-s", session_id, "shell", "echo", "-n", "$EXTERNAL_STORAGE/nopegl_data")
    dst_file = os.path.join(dst_dir, ofile)
    _exec("adb", "-s", session_id, "shell", "mkdir", "-p", f"{dst_dir}")
    _exec("adb", "-s", session_id, "push", "--sync", f"{ifile}", f"{dst_file}")
    return dst_file


def scene_change(session_id, scenefile, clear_color, samples):
    dst_dir = _exec("adb", "-s", session_id, "shell", "echo", "-n", "$EXTERNAL_STORAGE/nopegl_data")
    dst_file = os.path.join(dst_dir, os.path.basename(scenefile))
    _exec("adb", "-s", session_id, "shell", "mkdir", "-p", f'"{dst_dir}"')
    _exec("adb", "-s", session_id, "push", "--sync", f"{scenefile}", f"{dst_file}")
    # fmt: off
    _exec(
        'adb', '-s', session_id, 'shell', 'am', 'broadcast',
        '-a', 'scene_update',
        '--es', 'scene', f'{dst_file}',
        '--es', 'clear_color', f'%08X' % clear_color,
        '--ei', 'samples', '%d' % samples,
    )
    # fmt: off
