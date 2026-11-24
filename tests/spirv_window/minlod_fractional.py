"""Compare fractional point-filter MinLod on the same adapter."""
import sys

from minlod_levels import main


if __name__ == "__main__":
    args = sys.argv[1:]
    strict_rounding = bool(args and args[-1] == "--strict-rounding")
    if strict_rounding:
        args.pop()
    sys.exit(main(*args, fractional=True, strict_rounding=strict_rounding))
