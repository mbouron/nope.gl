import os
import os.path as op

from pynopegl_utils.misc import get_nopegl_tempdir


def main():
    report_dir = os.environ.get("REPORT_DIR", op.join(get_nopegl_tempdir(), "report"))
    print(f"Report: file://{report_dir}/")


if __name__ == "__main__":
    main()
