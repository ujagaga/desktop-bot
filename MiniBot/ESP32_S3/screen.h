#ifndef MINIBOT_SCREEN_H
#define MINIBOT_SCREEN_H

// Display policy: status screen after boot/wake until 2 s after the robot has its IP,
// then the neutral face, replaced by the sad face and a text line while an error lasts.
void SCREEN_StartWake();
void SCREEN_Process();
// Redraw what the policy wants now, e.g. after a voice session used the screen.
void SCREEN_Refresh();

#endif
