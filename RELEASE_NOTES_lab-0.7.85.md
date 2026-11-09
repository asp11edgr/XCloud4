# XCloud4 Lab 0.7.85 · APP_VER 01.53

- Experimental video recovery choices: NACK only, NACK + RTX, or NACK + RTX + FlexFEC. NACK only remains the default; negotiation reports what Xbox actually accepts.
- Persistent Lab-only simulated video packet loss of 0%, 1%, or 3%, captured for the next session. The default is 0%.
- Synced VSync remains available in the Lab. The public edition returns saved Synced selections to Automatic.
- Microphone capture remembers an unsupported 48kHz result for the rest of the process and uses the existing 16kHz path. Missing devices are retried every two seconds; port and silent-state diagnostics are emitted only on changes.
- Runtime acceptance and recovery behavior require the owner's console test; host results are documented separately.
