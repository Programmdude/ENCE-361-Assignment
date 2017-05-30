/**
 * @file buttons.h
 *
 * @author Daniel van Wichen
 * @date 2017-05-27
 */

#ifndef BUTTONS_H_
#define BUTTONS_H_

/*
 * State changes only after so many consecutive readings have an opposite value.
 */
#define NUM_POLLS 5

enum {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, NUM_BUTTONS};
enum {RELEASED, PUSHED};

/**
 * Initialise the buttons.
 */
void ButtonsInit(void);

/**
 * Update all of the buttons and their state.
 */
void UpdateButtons();

/**
 * Gets the number of pushes for a given button and resets the push count.
 *
 * @param button_name one of BUT_UP, BUT_DOWN, BUT_LEFT or BUT_RIGHT
 * @return the number of pushes for the given button since last called
 */
uint8_t NumPushes(uint8_t button_name);

/**
 * Reset the push count for all buttons.
 */
void ResetPushes(void);

#endif /* BUTTONS_H_ */
