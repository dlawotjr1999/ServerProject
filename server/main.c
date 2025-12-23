#include "net.h"

int main() {
	if(net_init() < 0)
		return 1;
	net_run();
	return 0;
}
