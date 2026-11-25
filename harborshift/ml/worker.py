"""Read model feature objects and return one prediction per input line."""
import json
import sys
from dataclasses import asdict

from .predict import predict_delay


def main():
    for line in sys.stdin:
        try:
            prediction = asdict(predict_delay(json.loads(line)))
            print(json.dumps(prediction, allow_nan=False), flush=True)
        except Exception:
            # The C caller applies its port-reopening rule on inference failure.
            print("null", flush=True)


if __name__ == "__main__":
    main()
