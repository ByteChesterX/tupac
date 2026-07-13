CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2
LDFLAGS = -lncursesw
TARGET = tupac
SRC = src/main.cpp

$(TARGET): $(SRC)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -o bin/$(TARGET) $(SRC) $(LDFLAGS)

release: $(SRC)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -s -o bin/$(TARGET) $(SRC) $(LDFLAGS)

clean:
	rm -rf bin

install: $(TARGET)
	install -Dm755 bin/$(TARGET) /usr/local/bin/$(TARGET)

uninstall:
	rm -f /usr/local/bin/$(TARGET)
	rm -f ~/.local/share/applications/tupac.desktop
	rm -f ~/.local/share/icons/tupac.svg

.PHONY: clean install uninstall release
