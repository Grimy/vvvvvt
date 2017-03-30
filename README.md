vvvvvt
======

Vivacious Varicolored Vernacular Verisimilar Virtual Terminal — a virtual terminal emulator for Linux.

Features
--------

* Vivacious
  * Extremely performant: `seq 10000000` runs about 50x faster than on XTerm<sup>[[1]](#footnote-1)</sup>
  * No tearing or flickering while scrolling
  * The release binary is only 33K

* Varicolored
  * 256 colors
  * Support for TrueType fonts
  * Bold, italic, underline, strikethrough…
  * Blinking is *not* supported (this is considered a feature)

* Vernacular
  * UTF-8 everywhere
  * Broken UTF-8 sequences are displayed as ⁇
  * …but the exact bytes are stored, letting you copy-paste non-UTF-8 text out of vvvvvt
  * No support for double-width characters at the moment

* Verisimilar
  * Uses the same [control sequences](http://invisible-island.net/xterm/ctlseqs/ctlseqs.html) as XTerm
  * All clients should behave identically when run in vvvvvt or XTerm<sup>[[2]](#footnote-2)</sup>

<a name=footnote-1>[1]</a>: XTerm has an off-by-default `fastScroll` option.
When turned on, XTerm is only 10x slower than vvvvvt, but at the cost
of not updating the screen at all while scrolling. Here are some timings on my system:

    $ cat test
    #!/bin/sh
    seq 1000000
    $ time vvvvvt ./test
    vvvvvt ./test  0.17s user 0.09s system 75% cpu 0.343 total
    $ time xterm ./test
    xterm ./test  5.99s user 1.03s system 37% cpu 18.674 total
    $ echo 'XTerm*fastScroll: on' | xrdb -merge
    $ time xterm ./test
    xterm ./test  1.63s user 0.97s system 105% cpu 2.474 total

<a name=footnote-2>[2]</a>: If you find an application that behaves differently
in vvvvvt, please [file a bug](https://github.com/Grimy/vvvvvt/issues/new)!

Installing
----------

Install the dependencies, using your package manager of choice:

```sh
pacman -S libxft fontconfig xsel
apt-get install libxft-dev libfontconfig-dev xsel
yum install libXft-devel fontconfig-devel xsel     # EPEL required for xsel
# etc…
```

Then download and build vvvvvt:

```sh
git clone https://github.com/Grimy/vvvvvt
cd vvvvvt
make
sudo make install
```

Configuring
-----------

vvvvvt is entirely configured through X resources.

TODO TODO

License
-------

vvvvvt is under the MIT license; see the [LICENSE](LICENSE) for details.
