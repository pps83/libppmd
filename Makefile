TARGET = ppmdi
SRC = ppmdi.c lib/Ppmd8.c lib/Ppmd8Dec.c lib/Ppmd8Enc.c
CFLAGS = -Ilib -O3 -march=native -flto -fwhole-program -funroll-loops -fno-plt -DNDEBUG -Wall -Wextra

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

$(TARGET): $(SRC)
	cc $(CFLAGS) $(SRC) -o $(TARGET)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	rm -f $(TARGET)
