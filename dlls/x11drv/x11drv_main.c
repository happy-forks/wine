/*
 * X11DRV initialization code
 *
 * Copyright 1998 Patrik Stridvall
 * Copyright 2000 Alexandre Julliard
 */

#include "config.h"

#ifdef NO_REENTRANT_X11
/* Get pointers to the static errno and h_errno variables used by Xlib. This
   must be done before including <errno.h> makes the variables invisible.  */
extern int errno;
static int *perrno = &errno;
extern int h_errno;
static int *ph_errno = &h_errno;
#endif  /* NO_REENTRANT_X11 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <X11/cursorfont.h>
#include "ts_xlib.h"
#include "ts_xutil.h"
#include "ts_shape.h"

#include "winbase.h"
#include "wine/winbase16.h"
#include "winreg.h"

#include "debugtools.h"
#include "gdi.h"
#include "options.h"
#include "user.h"
#include "win.h"
#include "wine_gl.h"
#include "x11drv.h"
#include "xvidmode.h"

DEFAULT_DEBUG_CHANNEL(x11drv);

static XKeyboardState keyboard_state;
static void (*old_tsx11_lock)(void);
static void (*old_tsx11_unlock)(void);

static CRITICAL_SECTION X11DRV_CritSection = CRITICAL_SECTION_INIT;

Display *display;
Screen *screen;
Visual *visual;
unsigned int screen_width;
unsigned int screen_height;
unsigned int screen_depth;
Window root_window;

unsigned int X11DRV_server_startticks;

#ifdef NO_REENTRANT_X11
static int* (*old_errno_location)(void);
static int* (*old_h_errno_location)(void);

/***********************************************************************
 *           x11_errno_location
 *
 * Get the per-thread errno location.
 */
static int *x11_errno_location(void)
{
    /* Use static libc errno while running in Xlib. */
    if (X11DRV_CritSection.OwningThread == GetCurrentThreadId()) return perrno;
    return old_errno_location();
}

/***********************************************************************
 *           x11_h_errno_location
 *
 * Get the per-thread h_errno location.
 */
static int *x11_h_errno_location(void)
{
    /* Use static libc h_errno while running in Xlib. */
    if (X11DRV_CritSection.OwningThread == GetCurrentThreadId()) return ph_errno;
    return old_h_errno_location();
}
#endif /* NO_REENTRANT_X11 */

/***********************************************************************
 *		error_handler
 */
static int error_handler(Display *display, XErrorEvent *error_evt)
{
    DebugBreak();  /* force an entry in the debugger */
    return 0;
}

/***********************************************************************
 *		lock_tsx11
 */
static void lock_tsx11(void)
{
    RtlEnterCriticalSection( &X11DRV_CritSection );
}

/***********************************************************************
 *		unlock_tsx11
 */
static void unlock_tsx11(void)
{
    RtlLeaveCriticalSection( &X11DRV_CritSection );
}

/***********************************************************************
 *		get_server_startup
 *
 * Get the server startup time 
 * Won't be exact, but should be sufficient
 */
static void get_server_startup(void)
{
    struct timeval t;
    gettimeofday( &t, NULL );
    X11DRV_server_startticks = ((t.tv_sec * 1000) + (t.tv_usec / 1000)) - GetTickCount();
}


/***********************************************************************
 *		setup_options
 *
 * Setup the x11drv options.
 */
static void setup_options(void)
{
    char buffer[256];
    HKEY hkey;
    DWORD type, count;

    if (RegCreateKeyExA( HKEY_LOCAL_MACHINE, "Software\\Wine\\Wine\\Config\\x11drv", 0, NULL,
                         REG_OPTION_VOLATILE, KEY_ALL_ACCESS, NULL, &hkey, NULL ))
    {
        ERR("Cannot create config registry key\n" );
        ExitProcess(1);
    }

    /* --display option */

    count = sizeof(buffer);
    if (!RegQueryValueExA( hkey, "display", 0, &type, buffer, &count ))
    {
        if (Options.display)
        {
            if (strcmp( buffer, Options.display ))
                MESSAGE( "%s: warning: --display option ignored, using '%s'\n", argv0, buffer );
        }
        else if ((Options.display = getenv( "DISPLAY" )))
        {
            if (strcmp( buffer, Options.display ))
                MESSAGE( "%s: warning: $DISPLAY variable ignored, using '%s'\n", argv0, buffer );
        }
        Options.display = strdup(buffer);
    }
    else
    {
        if (!Options.display && !(Options.display = getenv( "DISPLAY" )))
        {
            MESSAGE( "%s: no display specified\n", argv0 );
            ExitProcess(1);
        }
        RegSetValueExA( hkey, "display", 0, REG_SZ, Options.display, strlen(Options.display)+1 );
    }

    /* check and set --managed and --desktop options in wine config file
     * if it was not set on command line */

    if ((!Options.managed) && (Options.desktopGeometry == NULL))
    {
        count = sizeof(buffer);
        if (!RegQueryValueExA( hkey, "managed", 0, &type, buffer, &count ))
            Options.managed = IS_OPTION_TRUE( buffer[0] );

	count = sizeof(buffer);
        if (!RegQueryValueExA( hkey, "Desktop", 0, &type, buffer, &count ))
            /* Imperfect validation:  If Desktop=N, then we don't turn on
            ** the --desktop option.  We should really validate for a correct
            ** sizing entry */
            if (! IS_OPTION_FALSE(buffer[0]))
                Options.desktopGeometry = strdup(buffer);
    }

    if (Options.managed)
	RegSetValueExA( hkey, "managed", 0, REG_SZ, "y", 2 );

    if (Options.desktopGeometry)
        RegSetValueExA( hkey, "desktop", 0, REG_SZ, Options.desktopGeometry, strlen(Options.desktopGeometry)+1 );
    
    RegCloseKey( hkey );
}

   
/***********************************************************************
 *		create_desktop
 *
 * Create the desktop window for the --desktop mode.
 */
static void create_desktop( const char *geometry )
{
    int x = 0, y = 0, flags;
    unsigned int width = 640, height = 480;  /* Default size = 640x480 */
    char *name = "Wine desktop";
    XSizeHints *size_hints;
    XWMHints   *wm_hints;
    XClassHint *class_hints;
    XSetWindowAttributes win_attr;
    XTextProperty window_name;
    Atom XA_WM_DELETE_WINDOW;
    /* Used to create the desktop window with a good visual */
    XVisualInfo *vi = NULL;
#ifdef HAVE_OPENGL
    BOOL dblbuf_visual;
    int err_base, evt_base;
    
    /* Get in wine.ini if the desktop window should have a double-buffered visual or not.
       But first, test if OpenGL is even supported on the display ! */
    if (glXQueryExtension(display, &err_base, &evt_base) == True) {
      dblbuf_visual = PROFILE_GetWineIniBool( "x11drv", "DesktopDoubleBuffered", 0 );
      if (dblbuf_visual)  {
	int dblBuf[]={GLX_RGBA,GLX_DEPTH_SIZE,16,GLX_DOUBLEBUFFER,None};
	
	ENTER_GL();
	vi = glXChooseVisual(display, DefaultScreen(display), dblBuf);
	win_attr.colormap = XCreateColormap(display, RootWindow(display,vi->screen),
					    vi->visual, AllocNone);
	LEAVE_GL();
      }
    }
#endif /* HAVE_OPENGL */    

    flags = TSXParseGeometry( geometry, &x, &y, &width, &height );
    screen_width  = width;
    screen_height = height;

    /* Create window */
    win_attr.background_pixel = BlackPixel(display, 0);
    win_attr.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask |
                          PointerMotionMask | ButtonPressMask |
                          ButtonReleaseMask | EnterWindowMask;
    win_attr.cursor = TSXCreateFontCursor( display, XC_top_left_arrow );

    if (vi != NULL) {
      visual       = vi->visual;
      screen       = ScreenOfDisplay(display, vi->screen);
      screen_depth = vi->depth;
    }
    root_window = TSXCreateWindow( display,
                                   (vi == NULL ? DefaultRootWindow(display) : RootWindow(display, vi->screen)),
                                   x, y, width, height, 0,
                                   (vi == NULL ? CopyFromParent : vi->depth),
                                   InputOutput,
                                   (vi == NULL ? CopyFromParent : vi->visual),
                                   CWBackPixel | CWEventMask | CWCursor | (vi == NULL ? 0 : CWColormap),
                                   &win_attr );
  
    /* Set window manager properties */
    size_hints  = TSXAllocSizeHints();
    wm_hints    = TSXAllocWMHints();
    class_hints = TSXAllocClassHint();
    if (!size_hints || !wm_hints || !class_hints)
    {
        MESSAGE("Not enough memory for window manager hints.\n" );
        ExitProcess(1);
    }
    size_hints->min_width = size_hints->max_width = width;
    size_hints->min_height = size_hints->max_height = height;
    size_hints->flags = PMinSize | PMaxSize;
    if (flags & (XValue | YValue)) size_hints->flags |= USPosition;
    if (flags & (WidthValue | HeightValue)) size_hints->flags |= USSize;
    else size_hints->flags |= PSize;

    wm_hints->flags = InputHint | StateHint;
    wm_hints->input = True;
    wm_hints->initial_state = NormalState;
    class_hints->res_name = (char *)argv0;
    class_hints->res_class = "Wine";

    TSXStringListToTextProperty( &name, 1, &window_name );
    TSXSetWMProperties( display, root_window, &window_name, &window_name,
                        NULL, 0, size_hints, wm_hints, class_hints );
    XA_WM_DELETE_WINDOW = TSXInternAtom( display, "WM_DELETE_WINDOW", False );
    TSXSetWMProtocols( display, root_window, &XA_WM_DELETE_WINDOW, 1 );
    TSXFree( size_hints );
    TSXFree( wm_hints );
    TSXFree( class_hints );

    /* Map window */
    TSXMapWindow( display, root_window );
}


/***********************************************************************
 *           X11DRV process initialisation routine
 */
static void process_attach(void)
{
    WND_Driver       = &X11DRV_WND_Driver;

    get_server_startup();
    setup_options();

    /* setup TSX11 locking */
#ifdef NO_REENTRANT_X11
    old_errno_location = (void *)InterlockedExchange( (PLONG)&wine_errno_location,
                                                      (LONG)x11_errno_location );
    old_h_errno_location = (void *)InterlockedExchange( (PLONG)&wine_h_errno_location,
                                                        (LONG)x11_h_errno_location );
#endif /* NO_REENTRANT_X11 */
    old_tsx11_lock    = wine_tsx11_lock;
    old_tsx11_unlock  = wine_tsx11_unlock;
    wine_tsx11_lock   = lock_tsx11;
    wine_tsx11_unlock = unlock_tsx11;

    /* Open display */

    if (!(display = TSXOpenDisplay( Options.display )))
    {
        MESSAGE( "%s: Can't open display: %s\n", argv0, Options.display );
        ExitProcess(1);
    }
    fcntl( ConnectionNumber(display), F_SETFD, 1 ); /* set close on exec flag */
    screen = DefaultScreenOfDisplay( display );
    visual = DefaultVisual( display, DefaultScreen(display) );
    root_window = DefaultRootWindow( display );

    /* Initialize screen depth */

    screen_depth = PROFILE_GetWineIniInt( "x11drv", "ScreenDepth", 0 );
    if (screen_depth)  /* depth specified */
    {
        int depth_count, i;
        int *depth_list = TSXListDepths(display, DefaultScreen(display), &depth_count);
        for (i = 0; i < depth_count; i++)
            if (depth_list[i] == screen_depth) break;
        TSXFree( depth_list );
        if (i >= depth_count)
	{
            MESSAGE( "%s: Depth %d not supported on this screen.\n", argv0, screen_depth );
            ExitProcess(1);
	}
    }
    else screen_depth = DefaultDepthOfScreen( screen );

    /* tell the libX11 that we will do input method handling ourselves
     * that keep libX11 from doing anything whith dead keys, allowing Wine
     * to have total control over dead keys, that is this line allows
     * them to work in Wine, even whith a libX11 including the dead key
     * patches from Th.Quinot (http://Web.FdN.FR/~tquinot/dead-keys.en.html)
     */
    TSXOpenIM( display, NULL, NULL, NULL);

    if (Options.synchronous) XSetErrorHandler( error_handler );

    screen_width  = WidthOfScreen( screen );
    screen_height = HeightOfScreen( screen );

    if (Options.desktopGeometry)
    {
        Options.managed = FALSE;
        create_desktop( Options.desktopGeometry );
    }

    /* initialize GDI */
    if(!X11DRV_GDI_Initialize())
    {
        MESSAGE( "%s: X11DRV Couldn't Initialize GDI.\n", argv0 );
        ExitProcess(1);
    }


    /* save keyboard setup */
    TSXGetKeyboardControl(display, &keyboard_state);

    /* initialize event handling */
    X11DRV_EVENT_Init();

#ifdef HAVE_LIBXXF86VM
    /* initialize XVidMode */
    X11DRV_XF86VM_Init();
#endif

    /* load display.dll */
    LoadLibrary16( "display" );
}


/***********************************************************************
 *           X11DRV process termination routine
 */
static void process_detach(void)
{
    /* restore keyboard setup */
    XKeyboardControl keyboard_value;
  
    keyboard_value.key_click_percent = keyboard_state.key_click_percent;
    keyboard_value.bell_percent      = keyboard_state.bell_percent;
    keyboard_value.bell_pitch        = keyboard_state.bell_pitch;
    keyboard_value.bell_duration     = keyboard_state.bell_duration;
    keyboard_value.auto_repeat_mode  = keyboard_state.global_auto_repeat;
  
    XChangeKeyboardControl(display, KBKeyClickPercent | KBBellPercent | 
                           KBBellPitch | KBBellDuration | KBAutoRepeatMode, &keyboard_value);

#ifdef HAVE_LIBXXF86VM
    /* cleanup XVidMode */
    X11DRV_XF86VM_Cleanup();
#endif

    /* cleanup GDI */
    X11DRV_GDI_Finalize();

    /* restore TSX11 locking */
    wine_tsx11_lock = old_tsx11_lock;
    wine_tsx11_unlock = old_tsx11_unlock;
#ifdef NO_REENTRANT_X11
    wine_errno_location = old_errno_location;
    wine_h_errno_location = old_h_errno_location;
#endif /* NO_REENTRANT_X11 */

#if 0  /* FIXME */
    /* close the display */
    XCloseDisplay( display );
    display = NULL;

    WND_Driver       = NULL;
#endif
}


/***********************************************************************
 *           X11DRV initialisation routine
 */
BOOL WINAPI X11DRV_Init( HINSTANCE hinst, DWORD reason, LPVOID reserved )
{
    switch(reason)
    {
    case DLL_PROCESS_ATTACH:
        process_attach();
        break;
    case DLL_PROCESS_DETACH:
        process_detach();
        break;
    }
    return TRUE;
}

/***********************************************************************
 *              X11DRV_GetScreenSaveActive
 *
 * Returns the active status of the screen saver
 */
BOOL X11DRV_GetScreenSaveActive(void)
{
    int timeout, temp;
    TSXGetScreenSaver(display, &timeout, &temp, &temp, &temp);
    return timeout != 0;
}

/***********************************************************************
 *              X11DRV_SetScreenSaveActive
 *
 * Activate/Deactivate the screen saver
 */
void X11DRV_SetScreenSaveActive(BOOL bActivate)
{
    if(bActivate)
        TSXActivateScreenSaver(display);
    else
        TSXResetScreenSaver(display);
}

/***********************************************************************
 *              X11DRV_GetScreenSaveTimeout
 *
 * Return the screen saver timeout
 */
int X11DRV_GetScreenSaveTimeout(void)
{
    int timeout, temp;
    TSXGetScreenSaver(display, &timeout, &temp, &temp, &temp);
    return timeout;
}

/***********************************************************************
 *              X11DRV_SetScreenSaveTimeout
 *
 * Set the screen saver timeout
 */
void X11DRV_SetScreenSaveTimeout(int nTimeout)
{
    /* timeout is a 16bit entity (CARD16) in the protocol, so it should
     * not get over 32767 or it will get negative. */
    if (nTimeout>32767) nTimeout = 32767;
    TSXSetScreenSaver(display, nTimeout, 60, DefaultBlanking, DefaultExposures);
}

/***********************************************************************
 *              X11DRV_IsSingleWindow
 */
BOOL X11DRV_IsSingleWindow(void)
{
    return (root_window != DefaultRootWindow(display));
}
