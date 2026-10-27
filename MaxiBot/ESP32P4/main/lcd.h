#pragma once

void lcd_init(void);

/* Clears the screen and shows up to four lines; NULL leaves a line blank.
 * The first line is drawn in the accent color. */
void lcd_text(const char *l0, const char *l1, const char *l2, const char *l3);
