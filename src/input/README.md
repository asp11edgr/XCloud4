# DualShock 4 input

`controller.c` initializes Pad and UserService, opens the initial user's controller and reads it once per frame. `pressed` contains transitions from released to pressed; `data` retains held buttons, stick positions and trigger values. On errors, input becomes neutral and the application retries opening the controller.

The owner confirmed the controller view and its use in the UI on PS4 12.00. Individual button, axis, trigger and reconnection behavior has not been checked separately. Xbox game input remains subsequent work.
