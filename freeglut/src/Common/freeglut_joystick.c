/*
 * freeglut_joystick.c
 *
 * Joystick handling code
 *
 * Copyright (c) 1999-2000 Pawel W. Olszta. All Rights Reserved.
 * Written by Steve Baker, <sjbaker1@airmail.net>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * PAWEL W. OLSZTA BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * FreeBSD port by Stephen Montgomery-Smith <stephen@math.missouri.edu>
 *
 * Redone by John Fay 2/4/04 with another look from the PLIB "js" library.
 *  Many thanks for Steve Baker for permission to pull from that library.
 */

#include <GL/freeglut.h>
#include "freeglut_internal.h"
#ifdef HAVE_SYS_PARAM_H
#    include <sys/param.h>
#endif

/*
 * Initial defines from "js.h" starting around line 33 with the existing "freeglut_joystick.c"
 * interspersed
 */

#if TARGET_HOST_MACINTOSH
#    include <InputSprocket.h>
#endif

#if TARGET_HOST_MAC_OSX
#    include <mach/mach.h>
#    include <IOKit/IOkitLib.h>
#    include <IOKit/hid/IOHIDLib.h>
#endif

#if TARGET_HOST_POSIX_X11
#    ifdef HAVE_SYS_IOCTL_H
#        include <sys/ioctl.h>
#    endif
#    ifdef HAVE_FCNTL_H
#        include <fcntl.h>
#    endif
#    ifdef HAVE_ERRNO_H
#        include <errno.h>
#        include <string.h>
#    endif
#    if defined(__FreeBSD__) || defined(__FreeBSD_kernel__) || defined(__NetBSD__)
/* XXX The below hack is done until freeglut's autoconf is updated. */
#        define HAVE_USB_JS    1

#        if defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
#            include <sys/joystick.h>
#        else
/*
 * XXX NetBSD/amd64 systems may find that they have to steal the
 * XXX /usr/include/machine/joystick.h from a NetBSD/i386 system.
 * XXX I cannot comment whether that works for the interface, but
 * XXX it lets you compile...(^&  I do not think that we can do away
 * XXX with this header.
 */
#            include <machine/joystick.h>         /* For analog joysticks */
#        endif
#        define JS_DATA_TYPE joystick
#        define JS_RETURN (sizeof(struct JS_DATA_TYPE))
#    endif

#    if defined(__linux__)
#        include <linux/joystick.h>

/* check the joystick driver version */
#        if defined(JS_VERSION) && JS_VERSION >= 0x010000
#            define JS_NEW
#        endif
#    else  /* Not BSD or Linux */
#        ifndef JS_RETURN

  /*
   * We'll put these values in and that should
   * allow the code to at least compile when there is
   * no support. The JS open routine should error out
   * and shut off all the code downstream anyway and if
   * the application doesn't use a joystick we'll be fine.
   */

  struct JS_DATA_TYPE
  {
    int buttons;
    int x;
    int y;
  };

#            define JS_RETURN (sizeof(struct JS_DATA_TYPE))
#        endif
#    endif
#endif

#define JS_TRUE  1
#define JS_FALSE 0

/* BSD defines from "jsBSD.cxx" around lines 42-270 */

#if defined(__NetBSD__) || defined(__FreeBSD__) || defined(__FreeBSD_kernel__)

#    ifdef HAVE_USB_JS
#        if defined(__NetBSD__)
/* XXX The below hack is done until freeglut's autoconf is updated. */
#            define HAVE_USBHID_H 1
#            ifdef HAVE_USBHID_H
#                include <usbhid.h>
#            else
#                include <usb.h>
#            endif
#        elif defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
#            ifdef HAVE_USBHID_H
#                include <usbhid.h>
#            else
#                include <libusbhid.h>
#            endif
#        endif
#        include <legacy/dev/usb/usb.h>
#        include <dev/usb/usbhid.h>

/* Compatibility with older usb.h revisions */
#        if !defined(USB_MAX_DEVNAMES) && defined(MAXDEVNAMES)
#            define USB_MAX_DEVNAMES MAXDEVNAMES
#        endif
#    endif

static int hatmap_x[9] = { 0, 0, 1, 1, 1, 0, -1, -1, -1 };
static int hatmap_y[9] = { 0, 1, 1, 0, -1, -1, -1, 0, 1 };
struct os_specific_s {
  char             fname [128 ];
  int              fd;
  int              is_analog;
  /* The following structure members are specific to analog joysticks */
  struct joystick  ajs;
#    ifdef HAVE_USB_JS
  /* The following structure members are specific to USB joysticks */
  struct hid_item *hids;
  int              hid_dlen;
  int              hid_offset;
  char            *hid_data_buf;
  int              axes_usage [ _JS_MAX_AXES ];
#    endif
  /* We keep button and axes state ourselves, as they might not be updated
   * on every read of a USB device
   */
  int              cache_buttons;
  float            cache_axes [ _JS_MAX_AXES ];
};

/* Idents lower than USB_IDENT_OFFSET are for analog joysticks. */
#    define USB_IDENT_OFFSET    2

#    define USBDEV "/dev/usb"
#    define UHIDDEV "/dev/uhid"
#    define AJSDEV "/dev/joy"

#    ifdef HAVE_USB_JS
/*
 * fghJoystickFindUSBdev (and its helper, fghJoystickWalkUSBdev) try to locate
 * the full name of a USB device. If /dev/usbN isn't readable, we punt and
 * return the uhidN device name. We warn the user of this situation once.
 */
static char *fghJoystickWalkUSBdev(int f, char *dev, char *out, int outlen)
{
  struct usb_device_info di;
  int i, a;
  char *cp;

  for (a = 1; a < USB_MAX_DEVICES; a++) {
    di.udi_addr = a;
    if (ioctl(f, USB_DEVICEINFO, &di) != 0)
      return NULL;
    for (i = 0; i < USB_MAX_DEVNAMES; i++)
      if (di.udi_devnames[i][0] &&
          strcmp(di.udi_devnames[i], dev) == 0) {
        cp =  calloc( 1, strlen(di.udi_vendor) + strlen(di.udi_product) + 2);
        strcpy(cp, di.udi_vendor);
        strcat(cp, " ");
        strcat(cp, di.udi_product);
        strncpy(out, cp, outlen - 1);
        out[outlen - 1] = 0;
        free( cp );
        return out;
      }
  }
  return NULL;
}

static int fghJoystickFindUSBdev(char *name, char *out, int outlen)
{
  int i, f;
  char buf[50];
  char *cp;
  static int protection_warned = 0;

  for (i = 0; i < 16; i++) {
    snprintf(buf, sizeof(buf), "%s%d", USBDEV, i);
    f = open(buf, O_RDONLY);
    if (f >= 0) {
      cp = fghJoystickWalkUSBdev(f, name, out, outlen);
      close(f);
      if (cp)
        return 1;
    }
#ifdef HAVE_ERRNO_H
    else if (errno == EACCES) {
      if (!protection_warned) {
        fgWarning ( "Can't open %s for read!", buf );
        protection_warned = 1;
      }
    }
#endif
  }
  return 0;
}

static int fghJoystickInitializeHID(struct os_specific_s *os,
       int *num_axes, int *num_buttons)
{
    int size, is_joystick;
#   ifdef HAVE_USBHID_H
        int report_id = 0;
#   endif
    struct hid_data *d;
    struct hid_item h;
    report_desc_t rd;

    if ( ( rd = hid_get_report_desc( os->fd ) ) == 0 )
    {
#ifdef HAVE_ERRNO_H
        fgWarning ( "error: %s: %s", os->fname, strerror( errno ) );
#else
        fgWarning ( "error: %s", os->fname );
#endif
        return FALSE;
    }

    os->hids = NULL;

#   ifdef HAVE_USBHID_H
        if( ioctl( os->fd, USB_GET_REPORT_ID, &report_id ) < 0)
        {
            /*** XXX {report_id} may not be the right variable? ***/
#ifdef HAVE_ERRNO_H
            fgWarning ( "error: %s%d: %s", UHIDDEV, report_id, strerror( errno ) );
#else
            fgWarning ( "error: %s%d", UHIDDEV, report_id );
#endif
            return FALSE;
        }

        size = hid_report_size( rd, hid_input, report_id );
#   else
        size = hid_report_size( rd, 0, hid_input );
#   endif
    os->hid_data_buf = calloc( 1, size );
    os->hid_dlen = size;

    is_joystick = 0;
#   ifdef HAVE_USBHID_H
        d = hid_start_parse( rd, 1 << hid_input, report_id );
#   else
        d = hid_start_parse( rd, 1 << hid_input );
#   endif
        while( hid_get_item( d, &h ) )
        {
            int usage, page, interesting_hid;

            page = HID_PAGE( h.usage );
            usage = HID_USAGE( h.usage );

            /* This test is somewhat too simplistic, but this is how MicroSoft
             * does, so I guess it works for all joysticks/game pads. */
            is_joystick = is_joystick ||
                ( h.kind == hid_collection &&
                  page == HUP_GENERIC_DESKTOP &&
                  ( usage == HUG_JOYSTICK || usage == HUG_GAME_PAD ) );

            if( h.kind != hid_input )
                continue;

            if( !is_joystick )
                continue;

            interesting_hid = TRUE;
            if( page == HUP_GENERIC_DESKTOP )
            {
                switch( usage )
                {
                case HUG_X:
                case HUG_RX:
                case HUG_Y:
                case HUG_RY:
                case HUG_Z:
                case HUG_RZ:
                case HUG_SLIDER:
                    if( *num_axes < _JS_MAX_AXES )
                    {
                        os->axes_usage[ *num_axes ] = usage;
                        ( *num_axes )++;
                    }
                    break;
                case HUG_HAT_SWITCH:
                    /* Allocate two axes for a hat */
                    if( *num_axes + 1 < _JS_MAX_AXES )
                    {
                        os->axes_usage[ *num_axes ] = usage;
                        (*num_axes)++;
                        os->axes_usage[ *num_axes ] = usage;
                        (*num_axes)++;
                    }
                    break;
                default:
                    interesting_hid = FALSE;
                    break;
                }
            }
            else if( page == HUP_BUTTON )
            {
                interesting_hid = ( usage > 0 ) &&
                    ( usage <= _JS_MAX_BUTTONS );

                if( interesting_hid && usage - 1 > *num_buttons )
                    *num_buttons = usage - 1;
            }

            if( interesting_hid )
            {
                h.next = os->hids;
                os->hids = calloc( 1, sizeof ( struct hid_item ) );
                *os->hids = h;
            }
        }
        hid_end_parse( d );

        return os->hids != NULL;
}
#    endif
#endif

/*
 * Functions associated with the "jsJoystick" class in PLIB
 */
#if TARGET_HOST_MAC_OSX
#define K_NUM_DEVICES   32
int numDevices;
io_object_t ioDevices[K_NUM_DEVICES];

static void fghJoystickFindDevices ( SFG_Joystick* joy, mach_port_t );
static CFDictionaryRef fghJoystickGetCFProperties ( SFG_Joystick* joy, io_object_t );

static void fghJoystickEnumerateElements ( SFG_Joystick* joy, CFTypeRef element );
/* callback for CFArrayApply */
static void fghJoystickElementEnumerator ( SFG_Joystick* joy, void *element, void* vjs );

static void fghJoystickAddAxisElement ( SFG_Joystick* joy, CFDictionaryRef axis );
static void fghJoystickAddButtonElement ( SFG_Joystick* joy, CFDictionaryRef button );
static void fghJoystickAddHatElement ( SFG_Joystick* joy, CFDictionaryRef hat );
#endif


/* External function declarations (mostly platform-specific) */
extern void fgPlatformJoystickRawRead( SFG_Joystick* joy, int* buttons, float* axes );
extern void fgPlatformJoystickOpen( SFG_Joystick* joy );
extern void fgPlatformJoystickInit( SFG_Joystick *fgJoystick[], int ident );
extern void fgPlatformJoystickClose ( int ident );

/*
 * The static joystick structure pointer
 */
#define MAX_NUM_JOYSTICKS  2
static SFG_Joystick *fgJoystick [ MAX_NUM_JOYSTICKS ];

/*
 * Read the raw joystick data
 */
static void fghJoystickRawRead( SFG_Joystick* joy, int* buttons, float* axes )
{
    int i;

    /* Defaults */
    if( buttons )
        *buttons = 0;

    if( axes )
        for( i = 0; i < joy->num_axes; i++ )
            axes[ i ] = 1500.0f;

    if( joy->error )
        return;

	fgPlatformJoystickRawRead ( joy, buttons, axes );
}

/*
 * Correct the joystick axis data
 */
static float fghJoystickFudgeAxis( SFG_Joystick* joy, float value, int axis )
{
    if( value < joy->center[ axis ] )
    {
        float xx = ( value - joy->center[ axis ] ) / ( joy->center[ axis ] -
                                                       joy->min[ axis ] );

        if( xx < -joy->saturate[ axis ] )
            return -1.0f;

        if( xx > -joy->dead_band [ axis ] )
            return 0.0f;

        xx = ( xx + joy->dead_band[ axis ] ) / ( joy->saturate[ axis ] -
                                                 joy->dead_band[ axis ] );

        return ( xx < -1.0f ) ? -1.0f : xx;
    }
    else
    {
        float xx = ( value - joy->center [ axis ] ) / ( joy->max[ axis ] -
                                                        joy->center[ axis ] );

        if( xx > joy->saturate[ axis ] )
            return 1.0f;

        if( xx < joy->dead_band[ axis ] )
            return 0.0f;

        xx = ( xx - joy->dead_band[ axis ] ) / ( joy->saturate[ axis ] -
                                                 joy->dead_band[ axis ] );

        return ( xx > 1.0f ) ? 1.0f : xx;
    }
}

/*
 * Read the corrected joystick data
 */
static void fghJoystickRead( SFG_Joystick* joy, int* buttons, float* axes )
{
    float raw_axes[ _JS_MAX_AXES ];
    int  i;

    if( joy->error )
    {
        if( buttons )
            *buttons = 0;

        if( axes )
            for ( i=0; i<joy->num_axes; i++ )
                axes[ i ] = 0.0f;
    }

    fghJoystickRawRead( joy, buttons, raw_axes );

    if( axes )
        for( i=0; i<joy->num_axes; i++ )
            axes[ i ] = fghJoystickFudgeAxis( joy, raw_axes[ i ], i );
}

/*
 * Happy happy happy joy joy joy (happy new year toudi :D)
 */


#if TARGET_HOST_MAC_OSX
/** open the IOKit connection, enumerate all the HID devices, add their
interface references to the static array. We then use the array index
as the device number when we come to open() the joystick. */
static int fghJoystickFindDevices ( SFG_Joystick *joy, mach_port_t masterPort )
{
    CFMutableDictionaryRef hidMatch = NULL;
    IOReturn rv = kIOReturnSuccess;

    io_iterator_t hidIterator;
    io_object_t ioDev;

    /* build a dictionary matching HID devices */
    hidMatch = IOServiceMatching(kIOHIDDeviceKey);

    rv = IOServiceGetMatchingServices(masterPort, hidMatch, &hidIterator);
    if (rv != kIOReturnSuccess || !hidIterator) {
      fgWarning( "no joystick (HID) devices found" );
      return;
    }

    /* iterate */
    while ((ioDev = IOIteratorNext(hidIterator))) {
        /* filter out keyboard and mouse devices */
        CFDictionaryRef properties = getCFProperties(ioDev);
        long usage, page;

        CFTypeRef refPage = CFDictionaryGetValue (properties, CFSTR(kIOHIDPrimaryUsagePageKey));
        CFTypeRef refUsage = CFDictionaryGetValue (properties, CFSTR(kIOHIDPrimaryUsageKey));
        CFNumberGetValue((CFNumberRef) refUsage, kCFNumberLongType, &usage);
        CFNumberGetValue((CFNumberRef) refPage, kCFNumberLongType, &page);

        /* keep only joystick devices */
        if ( ( page == kHIDPage_GenericDesktop ) && (
                            (usage == kHIDUsage_GD_Joystick)
                         || (usage == kHIDUsage_GD_GamePad)
                         || (usage == kHIDUsage_GD_MultiAxisController)
                         || (usage == kHIDUsage_GD_Hatswitch) /* last two necessary ? */
            /* add it to the array */
            ioDevices[numDevices++] = ioDev;
    }

    IOObjectRelease(hidIterator);
}

static CFDictionaryRef fghJoystickGetCFProperties ( SFG_Joystick *joy, io_object_t ioDev )
{
    IOReturn rv;
    CFMutableDictionaryRef cfProperties;

#if 0
    /* comment copied from darwin/SDL_sysjoystick.c */
    /* Mac OS X currently is not mirroring all USB properties to HID page so need to look at USB device page also
     * get dictionary for usb properties: step up two levels and get CF dictionary for USB properties
     */

    io_registry_entry_t parent1, parent2;

    rv = IORegistryEntryGetParentEntry (ioDev, kIOServicePlane, &parent1);
    if (rv != kIOReturnSuccess) {
        fgWarning ( "error getting device entry parent");
        return NULL;
    }

    rv = IORegistryEntryGetParentEntry (parent1, kIOServicePlane, &parent2);
    if (rv != kIOReturnSuccess) {
        fgWarning ( "error getting device entry parent 2");
        return NULL;
    }
#endif

    rv = IORegistryEntryCreateCFProperties( ioDev /*parent2*/,
        &cfProperties, kCFAllocatorDefault, kNilOptions);
    if (rv != kIOReturnSuccess || !cfProperties) {
        fgWarning ( "error getting device properties");
        return NULL;
    }

    return cfProperties;
}

static void fghJoystickElementEnumerator ( SFG_Joystick *joy, void *element, void* vjs )
{
      if (CFGetTypeID((CFTypeRef) element) != CFDictionaryGetTypeID()) {
            fgError ( "%s", "element enumerator passed non-dictionary value");
            return;
    }

      static_cast<jsJoystick*>(vjs)->parseElement ( (CFDictionaryRef) element );
}

/** element enumerator function : pass NULL for top-level*/
static void fghJoystickEnumerateElements ( SFG_Joystick *joy, CFTypeRef element )
{
      FREEGLUT_INTERNAL_ERROR_EXIT( (CFGetTypeID(element) == CFArrayGetTypeID(),
                                    "Joystick element type mismatch",
                                    "fghJoystickEnumerateElements" );

      CFRange range = {0, CFArrayGetCount ((CFArrayRef)element)};
      CFArrayApplyFunction((CFArrayRef) element, range,
            &fghJoystickElementEnumerator, joy );
}

static void fghJoystickAddAxisElement ( SFG_Joystick *joy, CFDictionaryRef axis )
{
    long cookie, lmin, lmax;
    int index = joy->num_axes++;

    CFNumberGetValue ((CFNumberRef)
        CFDictionaryGetValue ( axis, CFSTR(kIOHIDElementCookieKey) ),
        kCFNumberLongType, &cookie);

    joy->pJoystick.axisCookies[index] = (IOHIDElementCookie) cookie;

    CFNumberGetValue ((CFNumberRef)
        CFDictionaryGetValue ( axis, CFSTR(kIOHIDElementMinKey) ),
        kCFNumberLongType, &lmin);

    CFNumberGetValue ((CFNumberRef)
        CFDictionaryGetValue ( axis, CFSTR(kIOHIDElementMaxKey) ),
        kCFNumberLongType, &lmax);

    joy->min[index] = lmin;
    joy->max[index] = lmax;
    joy->dead_band[index] = 0.0;
    joy->saturate[index] = 1.0;
    joy->center[index] = (lmax + lmin) * 0.5;
}

static void fghJoystickAddButtonElement ( SFG_Joystick *joy, CFDictionaryRef button )
{
    long cookie;
    CFNumberGetValue ((CFNumberRef)
            CFDictionaryGetValue ( button, CFSTR(kIOHIDElementCookieKey) ),
            kCFNumberLongType, &cookie);

    joy->pJoystick.buttonCookies[num_buttons++] = (IOHIDElementCookie) cookie;
    /* anything else for buttons? */
}

static void fghJoystickAddHatElement ( SFG_Joystick *joy, CFDictionaryRef button )
{
    /* hatCookies[num_hats++] = (IOHIDElementCookie) cookie; */
    /* do we map hats to axes or buttons? */
}
#endif

/*
 *  Platform-Specific Code
 */

#if TARGET_HOST_MACINTOSH
void fgPlatformJoystickRawRead( SFG_Joystick* joy, int* buttons, float* axes )
{
    int i;

    if ( buttons )
    {
        *buttons = 0;

        for ( i = 0; i < joy->num_buttons; i++ )
        {
            UInt32 state;
            int err = ISpElement_GetSimpleState ( joy->pJoystick.isp_elem [ i + ISP_NUM_AXIS ], &state);
            ISP_CHECK_ERR(err)

            *buttons |= state << i;
        }
    }

    if ( axes )
    {
        for ( i = 0; i < joy->num_axes; i++ )
        {
            UInt32 state;
            int err = ISpElement_GetSimpleState ( joy->pJoystick.isp_elem [ i ], &state );
            ISP_CHECK_ERR(err)

            axes [i] = (float) state;
        }
    }
}


void fgPlatformJoystickOpen( SFG_Joystick* joy )
{
	int i = 0;
    OSStatus err;

    /* XXX FIXME: get joystick name in Mac */

    err = ISpStartup( );

    if( err == noErr )
    {
#define ISP_CHECK_ERR(x) if( x != noErr ) { joy->error = GL_TRUE; return; }

        joy->error = GL_TRUE;

        /* initialize the needs structure */
        ISpNeed temp_isp_needs[ ISP_NUM_NEEDS ] =
        {
          { "\pX-Axis",  128, 0, 0, kISpElementKind_Axis,   kISpElementLabel_None, 0, 0, 0, 0 },
          { "\pY-Axis",  128, 0, 0, kISpElementKind_Axis,   kISpElementLabel_None, 0, 0, 0, 0 },
          { "\pZ-Axis",    128, 0, 0, kISpElementKind_Axis,   kISpElementLabel_None, 0, 0, 0, 0 },
          { "\pR-Axis",    128, 0, 0, kISpElementKind_Axis,   kISpElementLabel_None, 0, 0, 0, 0 },
          { "\pAxis   4",  128, 0, 0, kISpElementKind_Axis,   kISpElementLabel_None, 0, 0, 0, 0 },
          { "\pAxis   5",  128, 0, 0, kISpElementKind_Axis,   kISpElementLabel_None, 0, 0, 0, 0 },
          { "\pAxis   6",  128, 0, 0, kISpElementKind_Axis,   kISpElementLabel_None, 0, 0, 0, 0 },
          { "\pAxis   7",  128, 0, 0, kISpElementKind_Axis,   kISpElementLabel_None, 0, 0, 0, 0 },
          { "\pAxis   8",  128, 0, 0, kISpElementKind_Axis,   kISpElementLabel_None, 0, 0, 0, 0 },

          { "\pButton 0",  128, 0, 0, kISpElementKind_Button, kISpElementLabel_Btn_Select, 0, 0, 0, 0 },
          { "\pButton 1",  128, 0, 0, kISpElementKind_Button, kISpElementLabel_Btn_Select, 0, 0, 0, 0 },
          { "\pButton 2",  128, 0, 0, kISpElementKind_Button, kISpElementLabel_Btn_Select, 0, 0, 0, 0 },
          { "\pButton 3",  128, 0, 0, kISpElementKind_Button, kISpElementLabel_Btn_Select, 0, 0, 0, 0 },
          { "\pButton 4",  128, 0, 0, kISpElementKind_Button, kISpElementLabel_Btn_Select, 0, 0, 0, 0 },
          { "\pButton 5",  128, 0, 0, kISpElementKind_Button, kISpElementLabel_Btn_Select, 0, 0, 0, 0 },
          { "\pButton 6",  128, 0, 0, kISpElementKind_Button, kISpElementLabel_Btn_Select, 0, 0, 0, 0 },
          { "\pButton 7",  128, 0, 0, kISpElementKind_Button, kISpElementLabel_Btn_Select, 0, 0, 0, 0 },
          { "\pButton 8",  128, 0, 0, kISpElementKind_Button, kISpElementLabel_Btn_Select, 0, 0, 0, 0 },
          { "\pButton 9",  128, 0, 0, kISpElementKind_Button, kISpElementLabel_Btn_Select, 0, 0, 0, 0 },
          { "\pButton 10", 128, 0, 0, kISpElementKind_Button, kISpElementLabel_Btn_Select, 0, 0, 0, 0 },
          { "\pButton 11", 128, 0, 0, kISpElementKind_Button, kISpElementLabel_Btn_Select, 0, 0, 0, 0 },
          { "\pButton 12", 128, 0, 0, kISpElementKind_Button, kISpElementLabel_Btn_Select, 0, 0, 0, 0 },
          { "\pButton 13", 128, 0, 0, kISpElementKind_Button, kISpElementLabel_Btn_Select, 0, 0, 0, 0 },
          { "\pButton 14", 128, 0, 0, kISpElementKind_Button, kISpElementLabel_Btn_Select, 0, 0, 0, 0 },
          { "\pButton 15", 128, 0, 0, kISpElementKind_Button, kISpElementLabel_Btn_Select, 0, 0, 0, 0 },
          { "\pButton 16", 128, 0, 0, kISpElementKind_Button, kISpElementLabel_Btn_Select, 0, 0, 0, 0 },
          { "\pButton 17", 128, 0, 0, kISpElementKind_Button, kISpElementLabel_Btn_Select, 0, 0, 0, 0 },
          { "\pButton 18", 128, 0, 0, kISpElementKind_Button, kISpElementLabel_Btn_Select, 0, 0, 0, 0 },
          { "\pButton 19", 128, 0, 0, kISpElementKind_Button, kISpElementLabel_Btn_Select, 0, 0, 0, 0 },
          { "\pButton 20", 128, 0, 0, kISpElementKind_Button, kISpElementLabel_Btn_Select, 0, 0, 0, 0 },
          { "\pButton 21", 128, 0, 0, kISpElementKind_Button, kISpElementLabel_Btn_Select, 0, 0, 0, 0 },
          { "\pButton 22", 128, 0, 0, kISpElementKind_Button, kISpElementLabel_Btn_Select, 0, 0, 0, 0 },
          { "\pButton 23", 128, 0, 0, kISpElementKind_Button, kISpElementLabel_Btn_Select, 0, 0, 0, 0 },
          { "\pButton 24", 128, 0, 0, kISpElementKind_Button, kISpElementLabel_Btn_Select, 0, 0, 0, 0 },
          { "\pButton 25", 128, 0, 0, kISpElementKind_Button, kISpElementLabel_Btn_Select, 0, 0, 0, 0 },
          { "\pButton 26", 128, 0, 0, kISpElementKind_Button, kISpElementLabel_Btn_Select, 0, 0, 0, 0 },
          { "\pButton 27", 128, 0, 0, kISpElementKind_Button, kISpElementLabel_Btn_Select, 0, 0, 0, 0 },
          { "\pButton 28", 128, 0, 0, kISpElementKind_Button, kISpElementLabel_Btn_Select, 0, 0, 0, 0 },
          { "\pButton 29", 128, 0, 0, kISpElementKind_Button, kISpElementLabel_Btn_Select, 0, 0, 0, 0 },
          { "\pButton 30", 128, 0, 0, kISpElementKind_Button, kISpElementLabel_Btn_Select, 0, 0, 0, 0 },
          { "\pButton 31", 128, 0, 0, kISpElementKind_Button, kISpElementLabel_Btn_Select, 0, 0, 0, 0 },
        };

        memcpy( joy->pJoystick.isp_needs, temp_isp_needs, sizeof (temp_isp_needs ) );


        /* next two calls allow keyboard and mouse to emulate other input
         * devices (gamepads, joysticks, etc)
         */
        /*
          err = ISpDevices_ActivateClass ( kISpDeviceClass_Keyboard );
          ISP_CHECK_ERR(err)


          err = ISpDevices_ActivateClass ( kISpDeviceClass_Mouse );
          ISP_CHECK_ERR(err)
        */

        err = ISpElement_NewVirtualFromNeeds( ISP_NUM_NEEDS,
                                              joy->pJoystick.isp_needs, joy->pJoystick.isp_elem,
                                              0 );
        ISP_CHECK_ERR( err )

        err = ISpInit( ISP_NUM_NEEDS, joy->pJoystick.isp_needs, joy->pJoystick.isp_elem,
                       'freeglut', nil, 0, 128, 0 );
        ISP_CHECK_ERR( err )

        joy->num_buttons = ISP_NUM_NEEDS - ISP_NUM_AXIS;
        joy->num_axes    = ISP_NUM_AXIS;

        for( i = 0; i < joy->num_axes; i++ )
        {
            joy->dead_band[ i ] = 0;
            joy->saturate [ i ] = 1;
            joy->center   [ i ] = kISpAxisMiddle;
            joy->max      [ i ] = kISpAxisMaximum;
            joy->min      [ i ] = kISpAxisMinimum;
        }

        joy->error = GL_FALSE;
    }
    else
        joy->num_buttons = joy->num_axes = 0;
}


void fgPlatformJoystickInit( SFG_Joystick *fgJoystick[], int ident )
{
    fgJoystick[ ident ]->id = ident;
    snprintf( fgJoystick[ ident ]->pJoystick.fname, sizeof(fgJoystick[ ident ]->pJoystick.fname), "/dev/js%d", ident ); /* FIXME */
    fgJoystick[ ident ]->error = GL_FALSE;
}


void fgPlatformJoystickClose ( int ident )
{
    ISpSuspend( );
    ISpStop( );
    ISpShutdown( );
}
#endif

#if TARGET_HOST_MAC_OSX
void fgPlatformJoystickRawRead( SFG_Joystick* joy, int* buttons, float* axes )
{
    int i;

    if ( buttons != NULL )
    {
        *buttons = 0;

        for ( i = 0; i < joy->num_buttons; i++ )
        {
            IOHIDEventStruct hidEvent;
            (*(joy->pJoystick.hidDev))->getElementValue ( joy->pJoystick.hidDev, joy->pJoystick.buttonCookies[i], &hidEvent );
            if ( hidEvent.value )
                *buttons |= 1 << i;
        }
    }

    if ( axes != NULL )
    {
        for ( i = 0; i < joy->num_axes; i++ )
        {
            IOHIDEventStruct hidEvent;
            (*(joy->pJoystick.hidDev))->getElementValue ( joy->pJoystick.hidDev, joy->pJoystick.axisCookies[i], &hidEvent );
            axes[i] = hidEvent.value;
        }
    }
}


void fgPlatformJoystickOpen( SFG_Joystick* joy )
{
    IOReturn rv;
    SInt32 score;
    IOCFPlugInInterface **plugin;

    HRESULT pluginResult;

    CFDictionaryRef props;
    CFTypeRef topLevelElement;

    if( joy->id >= numDevices )
    {
        fgWarning( "device index out of range in fgJoystickOpen()" );
        return;
    }

    /* create device interface */
    rv = IOCreatePlugInInterfaceForService( ioDevices[ joy->id ],
                                            kIOHIDDeviceUserClientTypeID,
                                            kIOCFPlugInInterfaceID,
                                            &plugin, &score );

    if( rv != kIOReturnSuccess )
    {
        fgWarning( "error creating plugin for io device" );
        return;
    }

    pluginResult = ( *plugin )->QueryInterface(
        plugin,
        CFUUIDGetUUIDBytes(kIOHIDDeviceInterfaceID),
        &( LPVOID )joy->pJoystick.hidDev
    );

    if( pluginResult != S_OK )
        fgWarning ( "QI-ing IO plugin to HID Device interface failed" );

    ( *plugin )->Release( plugin ); /* don't leak a ref */
    if( joy->pJoystick.hidDev == NULL )
        return;

    /* store the interface in this instance */
    rv = ( *( joy->pJoystick.hidDev ) )->open( joy->pJoystick.hidDev, 0 );
    if( rv != kIOReturnSuccess )
    {
        fgWarning( "error opening device interface");
        return;
    }

    props = getCFProperties( ioDevices[ joy->id ] );

    /* recursively enumerate all the bits */
    CFTypeRef topLevelElement =
        CFDictionaryGetValue( props, CFSTR( kIOHIDElementKey ) );
    enumerateElements( topLevelElement );

    CFRelease( props );
}


void fgPlatformJoystickInit( SFG_Joystick *fgJoystick[], int ident )
{
    fgJoystick[ ident ]->id = ident;
    fgJoystick[ ident ]->error = GL_FALSE;
    fgJoystick[ ident ]->num_axes = 0;
    fgJoystick[ ident ]->num_buttons = 0;

    if( numDevices < 0 )
    {
        /* do first-time init (since we can't over-ride jsInit, hmm */
        numDevices = 0;

        mach_port_t masterPort;
        IOReturn rv = IOMasterPort( bootstrap_port, &masterPort );
        if( rv != kIOReturnSuccess )
        {
            fgWarning( "error getting master Mach port" );
            return;
        }
        fghJoystickFindDevices( masterPort );
    }

    if ( ident >= numDevices )
    {
        fgJoystick[ ident ]->error = GL_TRUE;
        return;
    }

    /* get the name now too */
    CFDictionaryRef properties = getCFProperties( ioDevices[ ident ] );
    CFTypeRef ref = CFDictionaryGetValue( properties,
                                          CFSTR( kIOHIDProductKey ) );
    if (!ref)
        ref = CFDictionaryGetValue(properties, CFSTR( "USB Product Name" ) );

    if( !ref ||
        !CFStringGetCString( ( CFStringRef )ref, name, 128,
                             CFStringGetSystemEncoding( ) ) )
    {
        fgWarning( "error getting device name" );
        name[ 0 ] = '\0';
    }
}


void fgPlatformJoystickClose ( int ident )
{
    ( *( fgJoystick[ ident ]->pJoystick.hidDev ) )->
        close( fgJoystick[ ident ]->pJoystick.hidDev );
}
#endif

#if TARGET_HOST_POSIX_X11
void fgPlatformJoystickRawRead( SFG_Joystick* joy, int* buttons, float* axes )
{
    int status;

    int i;

#if defined(__FreeBSD__) || defined(__FreeBSD_kernel__) || defined(__NetBSD__)
    int len;

    if ( joy->pJoystick.os->is_analog )
    {
        int status = read ( joy->pJoystick.os->fd, &joy->pJoystick.os->ajs, sizeof(joy->pJoystick.os->ajs) );
        if ( status != sizeof(joy->pJoystick.os->ajs) ) {
            perror ( joy->pJoystick.os->fname );
            joy->error = GL_TRUE;
            return;
        }
        if ( buttons != NULL )
            *buttons = ( joy->pJoystick.os->ajs.b1 ? 1 : 0 ) | ( joy->pJoystick.os->ajs.b2 ? 2 : 0 );

        if ( axes != NULL )
        {
            axes[0] = (float) joy->pJoystick.os->ajs.x;
            axes[1] = (float) joy->pJoystick.os->ajs.y;
        }

        return;
    }

#  ifdef HAVE_USB_JS
    while ( ( len = read ( joy->pJoystick.os->fd, joy->pJoystick.os->hid_data_buf, joy->pJoystick.os->hid_dlen ) ) == joy->pJoystick.os->hid_dlen )
    {
        struct hid_item *h;

        for  ( h = joy->pJoystick.os->hids; h; h = h->next )
        {
            int d = hid_get_data ( joy->pJoystick.os->hid_data_buf, h );

            int page = HID_PAGE ( h->usage );
            int usage = HID_USAGE ( h->usage );

            if ( page == HUP_GENERIC_DESKTOP )
            {
                int i;
                for ( i = 0; i < joy->num_axes; i++ )
                    if (joy->pJoystick.os->axes_usage[i] == usage)
                    {
                        if (usage == HUG_HAT_SWITCH)
                        {
                            if (d < 0 || d > 8)
                                d = 0;  /* safety */
                            joy->pJoystick.os->cache_axes[i] = (float)hatmap_x[d];
                            joy->pJoystick.os->cache_axes[i + 1] = (float)hatmap_y[d];
                        }
                        else
                        {
                            joy->pJoystick.os->cache_axes[i] = (float)d;
                        }
                        break;
                    }
            }
            else if (page == HUP_BUTTON)
            {
               if (usage > 0 && usage < _JS_MAX_BUTTONS + 1)
               {
                   if (d)
                       joy->pJoystick.os->cache_buttons |=  (1 << ( usage - 1 ));
                   else
                       joy->pJoystick.os->cache_buttons &= ~(1 << ( usage - 1 ));
               }
            }
        }
    }
#    ifdef HAVE_ERRNO_H
    if ( len < 0 && errno != EAGAIN )
#    else
    if ( len < 0 )
#    endif
    {
        perror( joy->pJoystick.os->fname );
        joy->error = 1;
    }
    if ( buttons != NULL ) *buttons = joy->pJoystick.os->cache_buttons;
    if ( axes    != NULL )
        memcpy ( axes, joy->pJoystick.os->cache_axes, sizeof(float) * joy->num_axes );
#  endif
#endif

#ifdef JS_NEW

    while ( 1 )
    {
        status = read ( joy->pJoystick.fd, &joy->pJoystick.js, sizeof(struct js_event) );

        if ( status != sizeof( struct js_event ) )
        {
#  ifdef HAVE_ERRNO_H
            if ( errno == EAGAIN )
            {
                /* Use the old values */
                if ( buttons )
                    *buttons = joy->pJoystick.tmp_buttons;
                if ( axes )
                    memcpy( axes, joy->pJoystick.tmp_axes,
                            sizeof( float ) * joy->num_axes );
                return;
            }
#  endif

            fgWarning ( "%s", joy->pJoystick.fname );
            joy->error = GL_TRUE;
            return;
        }

        switch ( joy->pJoystick.js.type & ~JS_EVENT_INIT )
        {
        case JS_EVENT_BUTTON:
            if( joy->pJoystick.js.value == 0 ) /* clear the flag */
                joy->pJoystick.tmp_buttons &= ~( 1 << joy->pJoystick.js.number );
            else
                joy->pJoystick.tmp_buttons |= ( 1 << joy->pJoystick.js.number );
            break;

        case JS_EVENT_AXIS:
            if ( joy->pJoystick.js.number < joy->num_axes )
            {
                joy->pJoystick.tmp_axes[ joy->pJoystick.js.number ] = ( float )joy->pJoystick.js.value;

                if( axes )
                    memcpy( axes, joy->pJoystick.tmp_axes, sizeof(float) * joy->num_axes );
            }
            break;

        default:
            fgWarning ( "PLIB_JS: Unrecognised /dev/js return!?!" );

            /* use the old values */

            if ( buttons != NULL ) *buttons = joy->pJoystick.tmp_buttons;
            if ( axes    != NULL )
                memcpy ( axes, joy->pJoystick.tmp_axes, sizeof(float) * joy->num_axes );

            return;
        }

        if( buttons )
            *buttons = joy->pJoystick.tmp_buttons;
    }
#else

    status = read( joy->pJoystick.fd, &joy->pJoystick.js, JS_RETURN );

    if ( status != JS_RETURN )
    {
        fgWarning( "%s", joy->pJoystick.fname );
        joy->error = GL_TRUE;
        return;
    }

    if ( buttons )
#    if defined( __FreeBSD__ ) || defined(__FreeBSD_kernel__) || defined( __NetBSD__ )
        *buttons = ( joy->pJoystick.js.b1 ? 1 : 0 ) | ( joy->pJoystick.js.b2 ? 2 : 0 );  /* XXX Should not be here -- BSD is handled earlier */
#    else
        *buttons = joy->pJoystick.js.buttons;
#    endif

    if ( axes )
    {
        axes[ 0 ] = (float) joy->pJoystick.js.x;
        axes[ 1 ] = (float) joy->pJoystick.js.y;
    }
#endif
}


void fgPlatformJoystickOpen( SFG_Joystick* joy )
{
#if defined( __FreeBSD__ ) || defined(__FreeBSD_kernel__) || defined( __NetBSD__ )
	int i = 0;
       char *cp;
#endif
#ifdef JS_NEW
       unsigned char u;
#else
#  if defined( __linux__ ) || TARGET_HOST_SOLARIS
	int i = 0;
    int counter = 0;
#  endif
#endif

#if defined( __FreeBSD__ ) || defined(__FreeBSD_kernel__) || defined( __NetBSD__ )
    for( i = 0; i < _JS_MAX_AXES; i++ )
        joy->pJoystick.os->cache_axes[ i ] = 0.0f;

    joy->pJoystick.os->cache_buttons = 0;

    joy->pJoystick.os->fd = open( joy->pJoystick.os->fname, O_RDONLY | O_NONBLOCK);

#ifdef HAVE_ERRNO_H
    if( joy->pJoystick.os->fd < 0 && errno == EACCES )
        fgWarning ( "%s exists but is not readable by you", joy->pJoystick.os->fname );
#endif

    joy->error =( joy->pJoystick.os->fd < 0 );

    if( joy->error )
        return;

    joy->num_axes = 0;
    joy->num_buttons = 0;
    if( joy->pJoystick.os->is_analog )
    {
        FILE *joyfile;
        char joyfname[ 1024 ];
        int noargs, in_no_axes;

        float axes [ _JS_MAX_AXES ];
        int buttons[ _JS_MAX_AXES ];

        joy->num_axes    =  2;
        joy->num_buttons = 32;

        fghJoystickRawRead( joy, buttons, axes );
        joy->error = axes[ 0 ] < -1000000000.0f;
        if( joy->error )
            return;

        snprintf( joyfname, sizeof(joyfname), "%s/.joy%drc", getenv( "HOME" ), joy->id );

        joyfile = fopen( joyfname, "r" );
        joy->error =( joyfile == NULL );
        if( joy->error )
            return;

        noargs = fscanf( joyfile, "%d%f%f%f%f%f%f", &in_no_axes,
                         &joy->min[ 0 ], &joy->center[ 0 ], &joy->max[ 0 ],
                         &joy->min[ 1 ], &joy->center[ 1 ], &joy->max[ 1 ] );
        joy->error = noargs != 7 || in_no_axes != _JS_MAX_AXES;
        fclose( joyfile );
        if( joy->error )
            return;

        for( i = 0; i < _JS_MAX_AXES; i++ )
        {
            joy->dead_band[ i ] = 0.0f;
            joy->saturate [ i ] = 1.0f;
        }

        return;    /* End of analog code */
    }

#    ifdef HAVE_USB_JS
    if( ! fghJoystickInitializeHID( joy->pJoystick.os, &joy->num_axes,
                                    &joy->num_buttons ) )
    {
        close( joy->pJoystick.os->fd );
        joy->error = GL_TRUE;
        return;
    }

    cp = strrchr( joy->pJoystick.os->fname, '/' );
    if( cp )
    {
        if( fghJoystickFindUSBdev( &cp[1], joy->name, sizeof( joy->name ) ) ==
            0 )
            strcpy( joy->name, &cp[1] );
    }

    if( joy->num_axes > _JS_MAX_AXES )
        joy->num_axes = _JS_MAX_AXES;

    for( i = 0; i < _JS_MAX_AXES; i++ )
    {
        /* We really should get this from the HID, but that data seems
         * to be quite unreliable for analog-to-USB converters. Punt for
         * now.
         */
        if( joy->pJoystick.os->axes_usage[ i ] == HUG_HAT_SWITCH )
        {
            joy->max   [ i ] = 1.0f;
            joy->center[ i ] = 0.0f;
            joy->min   [ i ] = -1.0f;
        }
        else
        {
            joy->max   [ i ] = 255.0f;
            joy->center[ i ] = 127.0f;
            joy->min   [ i ] = 0.0f;
        }

        joy->dead_band[ i ] = 0.0f;
        joy->saturate[ i ] = 1.0f;
    }
#    endif
#endif

#if defined( __linux__ ) || TARGET_HOST_SOLARIS
    /* Default for older Linux systems. */
    joy->num_axes    =  2;
    joy->num_buttons = 32;

#    ifdef JS_NEW
    for( i = 0; i < _JS_MAX_AXES; i++ )
        joy->pJoystick.tmp_axes[ i ] = 0.0f;

    joy->pJoystick.tmp_buttons = 0;
#    endif

    joy->pJoystick.fd = open( joy->pJoystick.fname, O_RDONLY );

    joy->error =( joy->pJoystick.fd < 0 );

    if( joy->error )
        return;

    /* Set the correct number of axes for the linux driver */
#    ifdef JS_NEW
    /* Melchior Franz's fixes for big-endian Linuxes since writing
     *  to the upper byte of an uninitialized word doesn't work.
     *  9 April 2003
     */
    ioctl( joy->pJoystick.fd, JSIOCGAXES, &u );
    joy->num_axes = u;
    ioctl( joy->pJoystick.fd, JSIOCGBUTTONS, &u );
    joy->num_buttons = u;
    ioctl( joy->pJoystick.fd, JSIOCGNAME( sizeof( joy->name ) ), joy->name );
    fcntl( joy->pJoystick.fd, F_SETFL, O_NONBLOCK );
#    endif

    /*
     * The Linux driver seems to return 512 for all axes
     * when no stick is present - but there is a chance
     * that could happen by accident - so it's gotta happen
     * on both axes for at least 100 attempts.
     *
     * PWO: shouldn't be that done somehow wiser on the kernel level?
     */
#    ifndef JS_NEW
    counter = 0;

    do
    {
        fghJoystickRawRead( joy, NULL, joy->center );
        counter++;
    } while( !joy->error &&
             counter < 100 &&
             joy->center[ 0 ] == 512.0f &&
             joy->center[ 1 ] == 512.0f );

    if ( counter >= 100 )
        joy->error = GL_TRUE;
#    endif

    for( i = 0; i < _JS_MAX_AXES; i++ )
    {
#    ifdef JS_NEW
        joy->max   [ i ] =  32767.0f;
        joy->center[ i ] =      0.0f;
        joy->min   [ i ] = -32767.0f;
#    else
        joy->max[ i ] = joy->center[ i ] * 2.0f;
        joy->min[ i ] = 0.0f;
#    endif
        joy->dead_band[ i ] = 0.0f;
        joy->saturate [ i ] = 1.0f;
    }
#endif
}


void fgPlatformJoystickInit( SFG_Joystick *fgJoystick[], int ident )
{
#if defined( __FreeBSD__ ) || defined(__FreeBSD_kernel__) || defined( __NetBSD__ )
    fgJoystick[ ident ]->id = ident;
    fgJoystick[ ident ]->error = GL_FALSE;

    fgJoystick[ ident ]->pJoystick.os = calloc( 1, sizeof( struct os_specific_s ) );
    memset( fgJoystick[ ident ]->pJoystick.os, 0, sizeof( struct os_specific_s ) );
    if( ident < USB_IDENT_OFFSET )
        fgJoystick[ ident ]->pJoystick.os->is_analog = 1;
    if( fgJoystick[ ident ]->pJoystick.os->is_analog )
        snprintf( fgJoystick[ ident ]->pJoystick.os->fname, sizeof(fgJoystick[ ident ]->pJoystick.os->fname), "%s%d", AJSDEV, ident );
    else
        snprintf( fgJoystick[ ident ]->pJoystick.os->fname, sizeof(fgJoystick[ ident ]->pJoystick.os->fname), "%s%d", UHIDDEV,
                 ident - USB_IDENT_OFFSET );
#elif defined( __linux__ )
    fgJoystick[ ident ]->id = ident;
    fgJoystick[ ident ]->error = GL_FALSE;

    snprintf( fgJoystick[ident]->pJoystick.fname, sizeof(fgJoystick[ident]->pJoystick.fname), "/dev/input/js%d", ident );

    if( access( fgJoystick[ ident ]->pJoystick.fname, F_OK ) != 0 )
        snprintf( fgJoystick[ ident ]->pJoystick.fname, sizeof(fgJoystick[ ident ]->pJoystick.fname), "/dev/js%d", ident );
#endif
}


void fgPlatformJoystickClose ( int ident )
{
#if defined( __FreeBSD__ ) || defined(__FreeBSD_kernel__) || defined( __NetBSD__ )
    if( fgJoystick[ident]->pJoystick.os )
    {
        if( ! fgJoystick[ ident ]->error )
            close( fgJoystick[ ident ]->pJoystick.os->fd );
#ifdef HAVE_USB_JS
        if( fgJoystick[ ident ]->pJoystick.os->hids )
            free (fgJoystick[ ident ]->pJoystick.os->hids);
        if( fgJoystick[ ident ]->pJoystick.os->hid_data_buf )
            free( fgJoystick[ ident ]->pJoystick.os->hid_data_buf );
#endif
        free( fgJoystick[ident]->pJoystick.os );
	}
#endif

    if( ! fgJoystick[ident]->error )
         close( fgJoystick[ ident ]->pJoystick.fd );
}
#endif






static void fghJoystickOpen( SFG_Joystick* joy )
{
    /*
     * Default values (for no joystick -- each conditional will reset the
     * error flag)
     */
    joy->error = TRUE;
    joy->num_axes = joy->num_buttons = 0;
    joy->name[ 0 ] = '\0';

	fgPlatformJoystickOpen ( joy );

}

/*
 * This function replaces the constructor method in the JS library.
 */
static void fghJoystickInit( int ident )
{
    if( ident >= MAX_NUM_JOYSTICKS )
      fgError( "Too large a joystick number: %d", ident );

    if( fgJoystick[ ident ] )
        fgError( "illegal attempt to initialize joystick device again" );

    fgJoystick[ ident ] =
        ( SFG_Joystick * )calloc( sizeof( SFG_Joystick ), 1 );

    /* Set defaults */
    fgJoystick[ ident ]->num_axes = fgJoystick[ ident ]->num_buttons = 0;
    fgJoystick[ ident ]->error = GL_TRUE;

	fgPlatformJoystickInit( fgJoystick, ident );

    fghJoystickOpen( fgJoystick[ ident  ] );
}

/*
 * Try initializing all the joysticks (well, both of them)
 */
void fgInitialiseJoysticks ( void )
{
    if( !fgState.JoysticksInitialised )
    {
        int ident ;
        for ( ident = 0; ident < MAX_NUM_JOYSTICKS; ident++ )
            fghJoystickInit( ident );

        fgState.JoysticksInitialised = GL_TRUE;
    }
}


void fgJoystickClose( void )
{
    int ident ;
    for( ident = 0; ident < MAX_NUM_JOYSTICKS; ident++ )
    {
        if( fgJoystick[ ident ] )
        {
			fgPlatformJoystickClose ( ident );

            free( fgJoystick[ ident ] );
            fgJoystick[ ident ] = NULL;
            /* show joystick has been deinitialized */
        }
    }
}

/*
 * Polls the joystick and executes the joystick callback hooked to the
 * window specified in the function's parameter:
 */
void fgJoystickPollWindow( SFG_Window* window )
{
    float axes[ _JS_MAX_AXES ];
    int buttons;
    int ident;

    freeglut_return_if_fail( window );
    freeglut_return_if_fail( FETCH_WCB( *window, Joystick ) );

    for( ident = 0; ident < MAX_NUM_JOYSTICKS; ident++ )
    {
        if( fgJoystick[ident] )
        {
            fghJoystickRead( fgJoystick[ident], &buttons, axes );

            if( !fgJoystick[ident]->error )
                INVOKE_WCB( *window, Joystick,
                            ( buttons,
                              (int) ( axes[ 0 ] * 1000.0f ),
                              (int) ( axes[ 1 ] * 1000.0f ),
                              (int) ( axes[ 2 ] * 1000.0f ) )
                );
        }
    }
}

/*
 * Implementation for glutDeviceGet(GLUT_HAS_JOYSTICK)
 */
int fgJoystickDetect( void )
{
    int ident;

    fgInitialiseJoysticks ();

    if ( !fgState.JoysticksInitialised )
        return 0;

    for( ident=0; ident<MAX_NUM_JOYSTICKS; ident++ )
        if( fgJoystick[ident] && !fgJoystick[ident]->error )
            return 1;

    return 0;
}

/*
 * Joystick information functions
 */
int  glutJoystickGetNumAxes( int ident )
{
    FREEGLUT_EXIT_IF_NOT_INITIALISED ( "glutJoystickGetNumAxes" );
    return fgJoystick[ ident ]->num_axes;
}
int  glutJoystickGetNumButtons( int ident )
{
    FREEGLUT_EXIT_IF_NOT_INITIALISED ( "glutJoystickGetNumButtons" );
    return fgJoystick[ ident ]->num_buttons;
}
int  glutJoystickNotWorking( int ident )
{
    FREEGLUT_EXIT_IF_NOT_INITIALISED ( "glutJoystickNotWorking" );
    return fgJoystick[ ident ]->error;
}

float glutJoystickGetDeadBand( int ident, int axis )
{
    FREEGLUT_EXIT_IF_NOT_INITIALISED ( "glutJoystickGetDeadBand" );
    return fgJoystick[ ident ]->dead_band [ axis ];
}
void  glutJoystickSetDeadBand( int ident, int axis, float db )
{
    FREEGLUT_EXIT_IF_NOT_INITIALISED ( "glutJoystickSetDeadBand" );
    fgJoystick[ ident ]->dead_band[ axis ] = db;
}

float glutJoystickGetSaturation( int ident, int axis )
{
    FREEGLUT_EXIT_IF_NOT_INITIALISED ( "glutJoystickGetSaturation" );
    return fgJoystick[ ident ]->saturate[ axis ];
}
void  glutJoystickSetSaturation( int ident, int axis, float st )
{
    FREEGLUT_EXIT_IF_NOT_INITIALISED ( "glutJoystickSetSaturation" );
    fgJoystick[ ident ]->saturate [ axis ] = st;
}

void glutJoystickSetMinRange( int ident, float *axes )
{
    FREEGLUT_EXIT_IF_NOT_INITIALISED ( "glutJoystickSetMinRange" );
    memcpy( fgJoystick[ ident ]->min, axes,
            fgJoystick[ ident ]->num_axes * sizeof( float ) );
}
void glutJoystickSetMaxRange( int ident, float *axes )
{
    FREEGLUT_EXIT_IF_NOT_INITIALISED ( "glutJoystickSetMaxRange" );
    memcpy( fgJoystick[ ident ]->max, axes,
            fgJoystick[ ident ]->num_axes * sizeof( float ) );
}
void glutJoystickSetCenter( int ident, float *axes )
{
    FREEGLUT_EXIT_IF_NOT_INITIALISED ( "glutJoystickSetCenter" );
    memcpy( fgJoystick[ ident ]->center, axes,
            fgJoystick[ ident ]->num_axes * sizeof( float ) );
}

void glutJoystickGetMinRange( int ident, float *axes )
{
    FREEGLUT_EXIT_IF_NOT_INITIALISED ( "glutJoystickGetMinRange" );
    memcpy( axes, fgJoystick[ ident ]->min,
            fgJoystick[ ident ]->num_axes * sizeof( float ) );
}
void glutJoystickGetMaxRange( int ident, float *axes )
{
    FREEGLUT_EXIT_IF_NOT_INITIALISED ( "glutJoystickGetMaxRange" );
    memcpy( axes, fgJoystick[ ident ]->max,
            fgJoystick[ ident ]->num_axes * sizeof( float ) );
}
void glutJoystickGetCenter( int ident, float *axes )
{
    FREEGLUT_EXIT_IF_NOT_INITIALISED ( "glutJoystickGetCenter" );
    memcpy( axes, fgJoystick[ ident ]->center,
            fgJoystick[ ident ]->num_axes * sizeof( float ) );
}

/*** END OF FILE ***/
