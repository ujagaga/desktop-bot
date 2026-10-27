#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the microWakeWord "hey jarvis" listener task on the microphone. */
void wake_word_start(void);

/* Registers the `ww` console command: detection statistics since last call. */
void wake_word_register_command(void);

#ifdef __cplusplus
}
#endif
