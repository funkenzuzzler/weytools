# weytool - A tool for Weytec MK06 keyboards

## Abstract

weytool allows to download, upload, list and delete files on Weytec MK06 keyboards.
This can already be done with a USB stick connected to the keyboard, but it gets
annoying when experimenting with changes and cannot be used when the keyboard is locked.
I don't have a locked keyboard and don't want to lock it, but it would be interesting
whether this tool also works when the keyboard is PIN locked or the Setup menu is
disabled.

In order to use the tool, the keyboard needs to be connected via the serial Host Port
(A or B, I only used HPA). The serial host port A is available at the 15-pin Misc Port
on the connector Box, Pin 10 is TXD, Pin 11 is RXD.

If the keyboard is used without a Connector Box, the HPA signals are the CN36 connector
at Pin 3 (RXD) and Pin 21(TXD).

## File organization
Files on the Weytec are not stored in FAT or a similar filesystem, instead a custom
filesystem is used where files are organized by an index and subindex.

Common indexes are:

 * 1 - System configuration
 * 2 - User menu
 * 4 - BMP files
 * 8 - Keycode (KCT) files
 * 9 - Layer files
 * 10 - User macros
 * 16 - PinCode (can only be written, but not read)

The subindex is just a serial number for index where more than one file is allowed,
for example the Layer, Keycode and Bitmap/Icon files.

There are a lot more indexes, i.e. for VSS configurations.

## Examples
### Listing files in keyboard storage
 ```
 $ weytool -D /dev/ttyUSB2 -l
 Number Index SubIndex Name
      0     1        0 SystemSetup.sec
      1     4        0 BMP0.BMP
      2     8        1 RD2GR110.kct
      3     8        3 R3KGR110_22135.kct
      4     8        6 FXSUS111.kct
      5     8        8 RX3IN110.kct
      6     8        9 RDLIN110.kct
      7     8       10 PCSIN110.kct
      8     8       11 EBMIN110.kct
      9     8       12 ERXIN110.kct
     10     8       13 BLOIN110.kct
     11     8       14 GLTIN110.kct
     12     8       15 RTTIN110.kct
     13     8       16 INSIN110.kct
     14     8       17 BRNIN110.kct
     15     8       20 SUVGR110.kct
     16     8       21 DAVIN110.kct
     17     8       22 TVSMEA110.kct
     18     8       23 TVPCIN110.kct
     19     9       10 LAYER10.LAY
     20     9       11 LAYER11.LAY
     21     9       12 LAYER12.LAY
     22     9       13 LAYER13.LAY
     23     9       14 LAYER14.LAY
     24     9       15 LAYER15.LAY
     25     9       17 LAYER17.LAY
     26     9       18 LAYER18.LAY
     27     9       19 LAYER19.LAY
     28     9       20 LAYER20.LAY
     29     9       21 LAYER21.LAY
     30     9       22 LAYER22.LAY
     31     9       23 LAYER23.LAY
     32     9       24 LAYER24.LAY
     33     9       25 LAYER25.LAY
     34    10        0 Macros.mac
     35     9        3 LAYER03.LAY
     36     9        4 LAYER04.LAY
     37     9        6 LAYER06.LAY
     38     9        8 LAYER08.LAY
     39    16        0 PinCode.txt
     40     2        0 UserMenu.usc
 ```
### Download a file from the keyboard

Reading is done by giving the index and subindex number to weytool:
 ```
 $ ./weytool -D /dev/ttyUSB2 -r 10,0
 10,0: Macros.mac 32768 bytes
 $
 ```
This will download the file at index 10, subindex 0 which is named Macros.mac.

NOTE: At the moment it's not possible to download Bitmap files.

### Upload a file to the keyboard

This will upload Macros.mac at index 10, subindex 0:
 ```
 $ ./weytool -D /dev/ttyUSB2 -w 10,0,Macros.mac
 $
 ```
### Upload icons to keyboard

Default icons in the keyboard are:
```
PC bitmap, Windows 3.x format, 80 x 70 x 8, image size 5600, resolution 2835 x 2835 px/m, cbSize 6678, bits offset 1078
```

Custom icons can be uploaded with:
```
./weytool -D /dev/ttyUSB2 -w 5,22,icon.bmp
```

This example would upload icon.bmp to Icon slot 22. If there's an internal
icon present, it will no longer be available until you delete the custom
icon again.

## Notes from reverse engineering
HPA commands:
```
70 XX                                     clears some flag (sound related?)
72 XX                                     accepts 0-3, unknown
73                                        unknown
74 XX                                     set LCD brightness, 0xff - max, 0xfe restore, 0 min
75 XX                                     supposed to print a character?
76 00                                     clears forground
77 00                                     yet another clear screen
78 LL                                     set first page of layer LL and save
79 PP                                     set page PP of current Layer
7a RR GG BB?                              some color fill
7d RS                                     set res line R - line, S - state mask
7a RR GG BB                               fill lcd
a2 <magic:2> <idx:2> <len:4>              Download graphics / colorparm?
                                          magic = a054 BMP
                                                  0101 COLORPARM
                                                  0901 SYSTEM / USERSETUP
a3
a4 00                                     Read 16 bytes config
a4 01                                     Read full config
a5 <idx:2> <subidx:2> <filename:32> <len:4> <data> Write file
a6 <idx:2> <subidx:2>                     Read file, FF = File Id, 1 - Systemsetup, 2 Usermenu
a7 <idx:2>
a8 <idx:2> <subidx:2>                     Delete file
a9 00 00 00                               Get Directory listing

7f 10 XX YPOS XPOS                        gotoxy
7f 11 XX <bitmask>                        0x01 - inverted, 0x10 large
7f 12 XX                                  0x02 - normal font size, other = small
7f 13 XX                                  ignored
7f 14 LL PP                               Switch layer = LL / page = PP, Page 70-73 graphics
7f 15 LL PP                               clear page PP of layer LL
7f 16 SS DD                               copy layer SS to layer DD
7f 17                                     ignored
7f 18 XX XX                               Yet another layer switch
7f 19 <num>                               Attach to WS, 0x10 detach
7f 20 LED STATE                           Set Led State, 1 - Green, 2 Red
                                          00 - CAPS
                                          01 - Attach/Detach
                                          02 - Scroll Lock
                                          03 - Prev/Next
                                          04 - Num Lock
7f 30 XX <null terminated string>         print text on lcd
7f 40                                     connector box update?
7f 41 VV PP                               beep, VV = 0..11 Volume, 255 = User set volume, PP = pattern
7f 52                                     dump layer config
7f 5a                                     dump kct file
7f 5b                                     dump user text page
7f 5c                                     dump pin file
7f 5c                                     test status
7f 5d                                     info page
7f 5e                                     dump macros
7f 5f                                     dump pin pke file
7f 60                                     write bmp file
7f e0 gMk_eLeCtRoNiC-DeSiGn_gMbH-WeRnB    unlock special functions
7f e2                                     returns display brightness
7f e3 31 xx                               unknown, some fifo i/o
7f e4 31 c0 02                            reboot keyboard
7f e5                                     returns 7f e5 1a
7f e7 2f                                  delete icon files
7f e7 0e XX                               LED test
7f e7 13                                  delete systemconfig + kct
7f e7 14                                  delete systemconfig + userconfig
7f e7 15                                  delete macroconfig
7f e7 16                                  delete bitmaps
7f e7 17                                  delete vssemu layer
7f e7 18                                  ignored
7f e8                                     keyboard ID; returns '1' for MK06, 'P' for EK11
7f e9                                     get module versions
7f ea ID                                  start test id (only reinstall fs?, seems to need a unlock)
7f eb
7f ec <4k data>                           write product data
7f ee go-DynBl                            start dynamic bootloader
7f f0 mode-eth                            terminal mode eth



7f 21 00                                  return keyboard language
```

## Default Pins

There seem to be two default supervisor pins defined:
 ```
 vmcvmc
 euro08
 ``` 

### PIN IDs

* 0-15: WS0-15
  16: Global
  17: Setup
