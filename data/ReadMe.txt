/*
 * cIOS d2x v${RELEASE_TEXT}
 * Build: ${BUILD_INFO}
 *
 * d2x by davebaol, later releases by blackb0x.
 * This release is from the Monsterray fork:
 * https://github.com/Monsterray/d2x-cios
 *
 */


[ DISCLAIMER ]

  THIS APPLICATION COMES WITH NO WARRANTY AT ALL, NEITHER EXPRESSED NOR
  IMPLIED. NO ONE BUT YOURSELF IS RESPONSIBLE FOR ANY DAMAGE TO YOUR WII
  CONSOLE BECAUSE OF A IMPROPER USAGE OF THIS SOFTWARE.

  Installing a cIOS writes to your console's NAND. Have BootMii or
  Priiloader and a NAND backup before you start.


[ DESCRIPTION ]

  This is a Custom IOS, an IOS modified to add some new features
  not available in the official IOS.

  This IOS has been made to be used ONLY with homebrew software.

  The d2x cIOS is an enhanced version of the cIOSX rev21 by Waninkoko.
  To see what's new read the file Changelog.txt. More information:
  https://github.com/Monsterray/d2x-cios (this fork)
  https://github.com/wiidev/d2x-cios (upstream, and its wiki)


[ WHICH SLOT AND BASE ]

  A cIOS is installed into a slot (a number) on top of a base IOS. These
  are the usual choices:

    slot 249, base 56   most games and loaders (install this one first)
    slot 250, base 57   USB loaders
    slot 248, base 38   the few games that need it
    slot 251, base 58   USB through Nintendo's own USB 2.0 driver

  Any slot that is not in use will do; the installer asks.
  Keep a spare slot for tests, and do not overwrite a cIOS you rely on.


[ HOW TO INSTALL IT ]

  You can install d2x cios through the d2x-cios-installer or ModMii.

  d2x-cios-installer (the all-in-one zip, d2x-cios-installer-with-d2x-v...):

  - Extract the zip into the root of your SD card or USB device. It
    creates the folder /apps/d2x-cios-installer with everything in it,
    this ReadMe included. You are done with this part.
  - If you are installing on a vWii (Wii U), delete ciosmaps.xml in that
    folder and rename ciosmaps-vWii.xml to ciosmaps.xml.
  - Launch the Homebrew Channel, start the d2x cIOS installer and follow
    the instructions on the screen. The Wii needs a network connection,
    because the installer downloads the base IOS from Nintendo's update
    server when it needs one.

  d2x-cios-installer, with the cIOS-only zip (${RELEASE_NAME}.zip):

  - Download the d2x cIOS installer and extract it into the apps folder of
    your SD card or USB device:
    https://github.com/larsenv/d2x-cios-installer
  - Extract ${RELEASE_NAME}.zip into the folder /apps/d2x-cios-installer.
    NOTE: This will overwrite the file /apps/d2x-cios-installer/ciosmaps.xml
    possibly present in that folder. You might want to rename it before
    extracting the d2x package.
  - On a vWii, use ciosmaps-vWii.xml as ciosmaps.xml, as above.
  - Launch the Homebrew Channel, start the installer and follow the
    instructions on the screen.

  ModMii:

  - If you are going to install a d2x vWii edition don't use ModMii. In this
    case use the d2x-cios-installer (see above).
  - Download and install the latest ModMii on your PC:
    https://gbatemp.net/threads/modmii-for-windows.207126/
  - Put the files of ${RELEASE_NAME}.zip in ModMii's Support\d2x-beta
    folder, all in that one folder: the modules (*.app) and d2x-beta.bat
    from the folder ${RELEASE_NAME}, ciosmaps.xml, ciosmaps-vWii.xml,
    ReadMe.txt and Changelog.txt (maked2x.sh in the source makes this layout).
  - From ModMii's Main Menu, enter "4", then "beta"
  - Select the d2x beta you want to build
  - Mark some or all d2x cIOSs for download (i.e. "d2x")
  - Enter "D" then "Y" to build d2x beta cIOSs\WADs
  - Install cIOSs using a WAD Manager - i.e. WiiMod\MMM\YAWMM (available on
    ModMii's Download Page 2)


[ KNOWN LIMITS ]

  - A USB drive left stuck by a console reset (the reset came in the middle
    of a transfer) has to be unplugged before it works again. d2x now
    fails fast instead of hanging, but the Wii cannot switch its USB ports'
    power from software.
  - On a SanDisk "USB 3.2Gen1" flash drive, writes land stale data. This
    happens with every d2x version. Reads work. Do not install games to
    that drive from a loader.
  - Tested USB drives were all FAT32. NTFS, WBFS partitions and other drive
    models were not tested on a Wii.


[ KUDOS ]

- rodries, for the help with EHCI improvements.
- Crediar, for all I learned studying Sneek source code.
- Oggzee, for his brilliant fraglist.
- WiiPower, for the great help with ios reload block from usb.
- dragbe and NutNut, for their d2x cios installer.
- XFlak, for his wonderful ModMii which supported d2x wads since its birth.
  Without ModMii d2x cios would probably never have existed. Also, XFlak had
  the original idea to replace the buggy EHCI module of cIOSX rev21 with the
  working one from rev19.
- HackWii.it and GBAtemp.net communities, for their ideas and support.
- Totoro, for the official d2x logo
- ChaN, for his FatFs.
- Waninkoko, for his cIOSX rev21.
- Team Twiizers and devkitPRO devs for their great work in libogc.
- WiiGator, for his work in the DIP plugin.
- kwiirk, for his EHCI module.
- Hermes, for his EHCI improvements.
- neimod, for the Custom IOS module.
- Monsterray, for the USB and SD fixes and the tests in this fork.
- All the betatesters.
