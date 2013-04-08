from __future__ import division, print_function

import numpy
import math
import time
from ctypes import c_void_p
import logging
logger = logging.getLogger(__name__);

# pyside and OpenGL are required deps for this module (not for all of freud), but we don't want to burden user scripts
# with lots of additional imports. So try and import them and throw a warning up to the parent module to handle
try:
    import OpenGL
    from PySide import QtCore, QtGui, QtOpenGL
except ImportError:
    logger.warning('Either PySide or pyopengl is not available, aborting rt initialization');
    raise ImportWarning('PySide or pyopengl not available');

# set opengl options
OpenGL.FORWARD_COMPATIBLE_ONLY = True
OpenGL.ERROR_ON_COPY = True
OpenGL.INFO_LOGGING = False
# OpenGL.ERROR_CHECKING = False

# force gl logger to emit only warnings and above
gl_logger = logging.getLogger('OpenGL')
gl_logger.setLevel(logging.WARNING)

from OpenGL import GL as gl

from freud import qt;
from . import rastergl

## \package freud.viz.rt
#
# Real-time visualization Qt widgets and rendering routines. freud.qt.init_app() must be called prior to constructing 
# any class in rt.
#
# \note freud.viz.rt **requires** pyside and pyopengl. If these dependencies are not present, a warning is issued to the
# logger, but execution continues with freud.viz.rt = None.
#

null = c_void_p(0)

## Widget for rendering scenes in real-time
#
# GLWidget renders a Scene in real-time using OpenGL. It (currently) only offers 2D camera control. Updates to the
# camera are made directly in the reference scene, so code external to GLWidget that uses the same scene will render
# the same point of view.
#
# ### Controls:
# - Panning mode (indicated by an open hand mouse cursor)
#     - *Click and drag* to **translate** the camera's x,y coordinates
# - At any time
#     - *Turn the mouse wheel* to **zoom** (*hold ctrl* to make finer adjustments)
#
# \note On mac
# - *ctrl* is *command*
# - *meta* is *control*
#
# TODO: rename class?
# TODO: Add 3d in as an option, or a 2nd widget?
# TODO: What about scenes that have both 2d and 3d geometry?
#
class GLWidget(QtOpenGL.QGLWidget):
    # animation states the UI code can take
    ANIM_IDLE = 1;
    ANIM_PAN = 2;
    
    ## Create a GLWidget
    # \param scene the Scene to render
    # \param *args non-keyword args passed on to QGLWidget 
    # \param **kwargs keyword args passed on to QGLWidget 
    #
    def __init__(self, scene, *args, **kwargs):
        if not qt.is_initialized():
            raise RuntimeError('freud.qt.init_app() must be called before constructing a GLWidget');
        
        QtOpenGL.QGLWidget.__init__(self, *args, **kwargs)
        self.scene = scene;
        
        self.setCursor(QtCore.Qt.OpenHandCursor)
        
        # initialize state machine variables
        self._anim_state = GLWidget.ANIM_IDLE;
        self._prev_pos = numpy.array([0,0], dtype=numpy.float32);
        self._prev_time = time.time();
        self._pan_vel = numpy.array([0,0], dtype=numpy.float32);
        self._initial_pan_vel = numpy.array([0,0], dtype=numpy.float32);
        self._initial_pan_time = time.time();
        
        # timers for FPS
        self.last_time = time.time();
        self.frame_count = 0;
        self.timer_fps = QtCore.QTimer(self)
        self.timer_fps.timeout.connect(self.updateFPS)
        #self.timer_fps.start(500)
        
        self._timer_animate = QtCore.QTimer(self)
        self._timer_animate.timeout.connect(self.animate)
    
    ## \internal 
    # \brief Resize the GL viewport
    #
    # Set the gl viewport size and update the camera resolution
    #
    def resizeGL(self, w, h):
        gl.glViewport(0, 0, w, h)
        self.scene.camera.setAspect(w/h);
        self.scene.camera.resolution = h;
    
    ## \internal
    # \brief Paint the GL scene
    #
    # Clear the draw buffers and redraws the scene
    #
    def paintGL(self):
        self.frame_count += 1;
        
        gl.glClearColor(1.0, 1.0, 1.0, 0.0);
        gl.glClear(gl.GL_COLOR_BUFFER_BIT | gl.GL_DEPTH_BUFFER_BIT);
        self.draw_gl.draw(self.scene);

    ## \internal
    # \brief Initialize the OpenGL renderer
    #
    # Initializes OpenGL and prints information about it to logger.info.
    #
    def initializeGL(self):
        logger.info('OpenGL version: ' + gl.glGetString(gl.GL_VERSION))
        self.draw_gl = rastergl.DrawGL();

    ## \internal
    # \brief Animation slot
    #
    # Called at idle while animating. Currently, only panning is animated. If self._anim_state is ANIM_PAN
    # then the camera is panned for ~1 second after the animation starts. The camera velocity is decreased
    # on an exponential curve.
    #
    # To start the animation loop, mouseReleaseEvent sets the initial velocity, initial time, previous time,
    # and starts up the timer _timer_animate which calls this method on idle.
    #
    # Once the velocity reaches zero, _anim_state is set back to ANIM_IDLE and the timer is stopped.
    #
    def animate(self):
        # If we are idle, stop the timer
        if self._anim_state == GLWidget.ANIM_IDLE:
            self._timer_animate.stop();
        
        # If we are panning
        if self._anim_state == GLWidget.ANIM_PAN:
            # Decrease the pan velocity on an exponential curve
            cur_time = time.time();
            self._pan_vel = numpy.exp(-(cur_time - self._initial_pan_time)/0.1) * self._initial_pan_vel;
            
            # Compute a delta (in camera units) and move the camera
            delta = self._pan_vel * (cur_time - self._prev_time) * self.scene.camera.pixel_size;
            delta[0] = delta[0] * -1;
            self.scene.camera.position[0:2] += delta;
            self._prev_time = cur_time;
            
            # Go back to the idle state when we come to a stop
            if numpy.dot(self._pan_vel, self._pan_vel) < 100:
                self._anim_state = GLWidget.ANIM_IDLE;
            
            # Redraw the GL view
            self.updateGL();

    ## \internal
    # \brief Update FPS slot
    #
    # Called at the completion of a benchmark run to update the FPS counter. Sets the fps public member variable.
    #
    def updateFPS(self):
        cur_time = time.time();

        if self.frame_count > 0:
            elapsed_time = cur_time - self.last_time;
            # print(self.frame_count / elapsed_time, "FPS");
            self.frame_count = 0;
            
        self.last_time = cur_time;
    
    ## \internal
    # \brief Close event
    #
    # Releases OpenGL resources when the widget is closed. 
    #
    def closeEvent(self, event):
        # stop the animation loop
        self._anim_state = GLWidget.ANIM_IDLE;
        
        # make the gl context current and free resources
        self.makeCurrent();
        self.draw_gl.destroy();            

    ## \internal
    # \brief Key pressed event
    #
    # Currently does nothing
    #
    def keyPressEvent(self, event):
        pass

    ## \internal
    # \brief Key released event
    #
    # Currently does nothing
    #
    def keyReleaseEvent(self, event):
        pass
    
    ## \internal
    # \brief Handle mouse move (while dragging) event
    #
    # Update the camera position based on the movement from the previous position while dragging with the left mouse
    # button.
    #
    def mouseMoveEvent(self, event):

        if event.buttons() & QtCore.Qt.LeftButton:
            # update camera position
            cur_time = time.time();
            cur_pos = numpy.array([event.x(), event.y()], dtype=numpy.float32);
            delta = (cur_pos - self._prev_pos) * self.scene.camera.pixel_size;
            delta[0] = delta[0] * -1;
            self.scene.camera.position[0:2] += delta;
            
            # compute pan velocity in camera pixels/second
            self._pan_vel[:] = (cur_pos - self._prev_pos) / (cur_time - self._prev_time);
            
            self._prev_time = cur_time;
            self._prev_pos = cur_pos;
            
            # Redraw the GL view
            self.updateGL();
            
            event.accept();
        else:
            event.ignore();

    ## \internal
    # \brief Handle mouse press event
    #
    # Start mouse-control panning the camera when the left button is pressed.
    #
    def mousePressEvent(self, event):
        if event.button() == QtCore.Qt.LeftButton:
            # stop any running pan animations
            self._anim_state = GLWidget.ANIM_IDLE;
            
            # save the drag start position, time, and update the cursor
            self._prev_pos[0] = event.x();
            self._prev_pos[1] = event.y();
            self._prev_time = time.time();
            self._pan_vel[:] = 0;
            self.setCursor(QtCore.Qt.ClosedHandCursor)
            event.accept();
        else:
            event.ignore();

    ## \internal
    # \brief Handle mouse release event
    #
    # stop mouse-control panning the camera when the left button is released
    # and start the animated panning
    #
    def mouseReleaseEvent(self, event):
        if event.button() == QtCore.Qt.LeftButton:
            # start the animation loop, set the initial pan vel and time, and update the cursor
            self._anim_state = GLWidget.ANIM_PAN;
            self._initial_pan_vel[:] = self._pan_vel[:];
            self._initial_pan_time = self._prev_time;
            self._timer_animate.start()
            self.setCursor(QtCore.Qt.OpenHandCursor)
            event.accept();
        else:
            event.ignore();
    
    ## \internal
    # \brief Zoom in response to a mouse wheel event
    #
    def wheelEvent(self, event):
        # control speed based on modifiers (ctrl/cmd = slow)
        if event.modifiers() == QtCore.Qt.ControlModifier:
            speed = 0.05;
        else:
            speed = 0.2;
        
        if event.orientation() == QtCore.Qt.Vertical:
            # zoom the camera based on the mouse wheel. Zooming is a constant factor reduction (or increase) in size.
            f = 1 - speed * float(event.delta())/120;
            self.scene.camera.setHeight(self.scene.camera.getHeight() * f);
            
            # Redraw the GL view
            self.updateGL();
            event.accept();
    
class Window(QtGui.QWidget):
    def __init__(self, scene, *args, **kwargs):
        QtGui.QWidget.__init__(self, *args, **kwargs)

        self.glWidget = GLWidget(scene)

        mainLayout = QtGui.QHBoxLayout()
        mainLayout.addWidget(self.glWidget)
        self.setLayout(mainLayout)

        self.setWindowTitle("Hello World")


##########################################
# Module init

# set the default GL format
glFormat = QtOpenGL.QGLFormat();
glFormat.setVersion(2, 1);
glFormat.setProfile( QtOpenGL.QGLFormat.CompatibilityProfile );
glFormat.setSampleBuffers(True);
glFormat.setSwapInterval(0);
QtOpenGL.QGLFormat.setDefaultFormat(glFormat);
