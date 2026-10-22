# light_ai

AI-powered lighting control for RK3588 (Linaro/Debian).

The system uses a camera and artificial intelligence (neural networks)
to detect objects and people in the field of view — including people
approaching by their shadows before they enter the frame — and controls
lighting accordingly: switching lights on/off and adjusting brightness
based on real-time ambient light sensor readings and time of day.

## Key features

- People and object detection in camera view
- Shadow-based early presence detection (light turns on before a person enters the room)
- Adaptive brightness control driven by ambient light sensor data
- Time-of-day aware lighting logic

## Hardware

- RK3588 SBC (Debian/Linaro)
- Camera (TBD)
- Ambient light sensor (TBD)
- Light actuator: relay / dimmer (TBD)

## Status

Work in progress.
