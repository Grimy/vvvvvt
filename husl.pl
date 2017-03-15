#!/usr/bin/perl -w

use strict;
use Convert::Color::HUSL;

sub hsl2rgb   { [ map 65536 * $_, Convert::Color::HUSL->new(@$_)->rgb ] }
sub sixd2rgb  { $_ ? 257 * (55 + 40 * $_) : 0 }
sub grayscale { [(257 * (8 + 10 * $_)) x 3] }

print "static const XftColor colors[] = {\n";

printf "\t{ 0, { 0x%04x, 0x%04x, 0x%04x, 0xffff } },\n", @$_ for
map(hsl2rgb,
	[270, 11,  7],   # Black   Background
	[  0, 99, 61],   # Red     Error
	[120, 99, 61],   # Green   --
	[ 60, 99, 61],   # Brown   --
	[210, 99, 61],   # Azure   --
	[330, 99, 61],   # Pink    --
	[150, 99, 61],   # Spring  --
	[300, 11, 16],   # Dark    Cursorline
	[240, 11, 61],   # Gray    Comments
	[ 30, 99, 61],   # Orange  Todo
	[120, 99, 81],   # Green   Menus
	[ 60, 99, 81],   # Yellow  Keywords
	[240, 99, 81],   # Blue    String
	[270, 99, 61],   # Violet  PreProc
	[180, 99, 81],   # Cyan    --
	[300, 11, 96],   # White   Foreground
),
map([map sixd2rgb, $_ / 36 % 6, $_ / 6 % 6, $_ % 6], 0..216),
map(grayscale, 0..24);

print "};\n";
