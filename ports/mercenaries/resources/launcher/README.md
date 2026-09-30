# Launcher resources

The active launcher uses the supplied framed-window design and the embedded
Agency FB Regular font. PNG artwork is embedded unchanged. `setup_art.cpp` draws the
layout, text, progress bar and percentage; progress comes from the extraction
worker rather than an animation timer. Artwork is scaled proportionally.

`launcher.rc` defines these resources:

| ID | File | Use |
| --- | --- | --- |
| 201 | `background.png` | Helicopter artwork |
| 202 | `suits.png` | Card suits |
| 203 | `frame-close.png` | Enabled close button |
| 204 | `emblem.png` | Emblem |
| 205 | `badge-panel.png` | Emblem and suit panel |
| 206 | `frame-title.png` | Window title frame |
| 207 | `frame-body.png` | Window body frame |
| 208 | `frame-close-disabled.png` | Close button during installation |
| 209 | `AgencyFB-Regular.ttf` | Text font |
| 210 | `AgencyFB-COPYRIGHT.txt` | Font notice |

The font is loaded privately, without installing it on the user's system. Its
notice is copied to `tools/AgencyFB-COPYRIGHT.txt` in the player package. The red close
button is disabled during installation and restored when the worker finishes.

The earlier Anton and Hack fonts, their notices and the unframed panel/close artwork are
retained reference assets. They are not embedded by the current `launcher.rc`.
