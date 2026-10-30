#include "ui.h"

bool celosia::inputsystem::key::down(int key) {
	const unsigned char k = key & 0xFF;
	watch(k);
	return registry::state[k] && !registry::previous[k];
}

bool celosia::inputsystem::key::up(int key) {
	const unsigned char k = key & 0xFF;
	watch(k);
	return !registry::state[k] && registry::previous[k];
}

bool celosia::inputsystem::key::held(int key) { // key is currently down
	return platform::key_held(key);
}

void celosia::inputsystem::key::watch(int key) { // add key to the watchlist
	const unsigned char k = key & 0xFF;
	if (!registry::watched[k]) {
		registry::watched[k] = true;
		registry::state[k] = registry::previous[k] = key::held(k); // a key that's already down doesn't count as just pressed
	}
}

void celosia::inputsystem::key::unwatch(int key) { // remove key from the watchlist
	registry::watched[key & 0xFF] = false;
}

void celosia::inputsystem::refresh() { // loop through all of the keys in watchlist and validate them
	for (int k = 0; k < 256; k++)
		if (registry::watched[k]) {
			registry::previous[k] = registry::state[k];
			registry::state[k] = key::held(k);
		}
}
