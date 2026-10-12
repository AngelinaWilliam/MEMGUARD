#include "test_framework.hpp"

#include <cstring>

int main(int argc, char** argv)
{
	const char* filter = argc > 1 ? argv[1] : nullptr;
	int run = 0;
	int failedTests = 0;
	for (const auto& t : mgtest::registry())
	{
		if (filter && std::strstr(t.name, filter) == nullptr)
		{
			continue;
		}
		const int before = mgtest::failures();
		std::printf("[ RUN  ] %s\n", t.name);
		t.fn();
		++run;
		if (mgtest::failures() != before)
		{
			++failedTests;
			std::printf("[ FAIL ] %s\n", t.name);
		}
		else
		{
			std::printf("[  OK  ] %s\n", t.name);
		}
	}
	std::printf("\n%d test(s) run, %d failed, %d failed check(s)\n", run, failedTests, mgtest::failures());
	return failedTests == 0 && run > 0 ? 0 : 1;
}
