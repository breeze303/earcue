# Manual audio-engine checks

These are standalone C checks, not part of `swift build`.

## Feedback suppressor

```bash
clang -std=c11 -O2 -I Sources/EarCueAudio \
  Sources/EarCueAudio/FeedbackSuppressor.c tools/test_feedback.c \
  -lm -o /tmp/test_feedback
/tmp/test_feedback
```

## Core Audio engine

```bash
clang -std=c11 -O2 -I Sources/EarCueAudio/include -I Sources/EarCueAudio \
  -framework CoreAudio -framework AudioToolbox -framework CoreFoundation -framework Foundation \
  Sources/EarCueAudio/EarCueAudio.c Sources/EarCueAudio/FeedbackSuppressor.c \
  tools/test_engine.c -lm -o /tmp/test_engine
/tmp/test_engine "MacBook Air麦克风" Oray 2
```

The engine test routes a microphone to a virtual/output device; adjust the
device names for your machine.
