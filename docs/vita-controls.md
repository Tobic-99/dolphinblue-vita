# PS Vita controls

| Vita | Game (Dreamcast pad) |
|---|---|
| D-pad, left stick | D-pad |
| Cross | A |
| Circle | B |
| Square | X |
| Triangle | Y |
| L | C |
| R | Z |
| START | Start |
| SELECT (short press, alone) | pause / resume (the game and the sound stop, the last frame stays on screen) |
| SELECT + START held 2 s | delete the save state `state.bin` |
| SELECT held while the game starts | start without loading `state.bin` |

There is no coin input: the game runs in free play.

## Option files

Empty files in `ux0:data/dolphinblue/` change the defaults:

| File | Effect |
|---|---|
| `aspect43` | 4:3 picture with black bars instead of full screen |
| `nocap` | no 30 fps cap (frames are shown as soon as they are ready) |
| `nosavestates` | no save states (no automatic save per mission, no resume) |
| `tex32` | keep every texture at 32 bits (no 16-bit texture formats) |
| `noarmidle` | no idle skip in the sound driver's main loop |
| `gdsync` | synchronous disc reads |
