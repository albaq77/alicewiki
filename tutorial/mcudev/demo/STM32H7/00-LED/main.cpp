#include <cpp/MCU/ST/STM32H7>
extern "C" char _IDN_BOARD[16] {"STM32H743IIT6"};

using namespace uni;

// Not Advertisement: Openedv Board Parameters used
GPIN& LEDB = GPIOB[0];
GPIN& LEDR = GPIOB[1];

stduint con0_out;

int main() {
	LEDB.setMode(GPIOMode::OUT);
	LEDR.setMode(GPIOMode::OUT);
	while (true) {
		LEDR.Toggle();
		for(volatile unsigned i{0}; i < 1000000; i++){}
		LEDB.Toggle();
		for(volatile unsigned i{0}; i < 1000000; i++){}
	}
}

void erro(const char* erromsg){ }
