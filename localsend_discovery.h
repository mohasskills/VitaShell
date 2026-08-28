#ifndef DISCOVERY_H
#define DISCOVERY_H

void discovery_init();
void discovery_broadcast();
void discovery_poll();       // call each frame to receive peer announcements
void discovery_term();

#endif // DISCOVERY_H
