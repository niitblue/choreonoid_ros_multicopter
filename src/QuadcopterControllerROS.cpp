/**
   Copyright (c) 2018 Japan Atomic Energy Agency (JAEA).
   The original version is implemented as an RT-component.
   This is a simple controller version modified by AIST.
*/

#include <cnoid/SimpleController>
#include <cnoid/RotorDevice>
#include <cnoid/SharedJoystick>
#include <cnoid/EigenUtil>

#include <ros/node_handle.h>
#include <sensor_msgs/Joy.h>
#include <mutex>

using namespace std;
using namespace cnoid;

namespace {

const string propname[] = { "PROP0", "PROP1", "PROP2", "PROP3" };
const std::string rotorname[] = { "RotorDevice0", "RotorDevice1", "RotorDevice2", "RotorDevice3" };

#define L_STICK_H_AXIS 0
#define L_STICK_V_AXIS 1
#define R_STICK_H_AXIS 3
#define R_STICK_V_AXIS 4

#define L_TRIGGER_AXIS 2
#define DIRECTIONAL_PAD_V_AXIS 7

#define A_BUTTON 0
#define POWER_BUTTON A_BUTTON

#define R_STICK_BUTTON 12

const int rotorAxis[] = {
    L_STICK_V_AXIS,
    R_STICK_H_AXIS,
    R_STICK_V_AXIS,
    L_STICK_H_AXIS
};


int cameraAxis = DIRECTIONAL_PAD_V_AXIS;
const int powerButton = A_BUTTON;

const double sign[4][4] = {
    { 1.0, -1.0, -1.0,  1.0 },
    { 1.0,  1.0, -1.0, -1.0 },
    { 1.0,  1.0,  1.0,  1.0 },
    { 1.0, -1.0,  1.0, -1.0 }
};
const double dir[] = { 1.0, -1.0, 1.0, -1.0 };

static const double KP[] = { 0.4, 0.4, 0.4, 1.0 };
static const double KD[] = { 0.02, 1.0, 1.0, 0.05 };
const double RATE[] = { -1.0, 0.1, -0.1, -0.5 };

// For the stable mode
const double KPX[] = { 0.4, 0.4 };
const double KDX[] = { 0.4, 0.4 };
const double RATEX[] = { -1.0, -1.0 };

class QuadcopterControllerROS : public SimpleController
{
public:
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW

    std::unique_ptr<ros::NodeHandle> node;
    ros::Subscriber joystickSubscriber;
    sensor_msgs::Joy latestJoystickState;
    std::mutex joystickMutex;

    ///SharedJoystickPtr joystick;
    int targetMode;
    BodyPtr ioBody;
    ostream* os;
    Link* prop[4];
    Multicopter::RotorDevice* rotor[4];
    Link* cameraT;
    double timeStep;

    Vector4 zrpyref;
    Vector4 zrpyprev;
    Vector4 dzrpyref;
    Vector4 dzrpyprev;

    // For the stable mode
    bool isStableMode;
    bool prevModeButtonState;
    Vector2 xyref;
    Vector2 xyprev;
    Vector2 dxyref;
    Vector2 dxyprev;
    
    double qref;
    double qprev;
    bool power;
    bool powerprev;
    bool rotorswitch;

    virtual bool configure(SimpleControllerConfig* config) override;
    virtual bool initialize(SimpleControllerIO* io) override;
    Vector4 getZRPY();
    Vector2 getXY();
    virtual bool control() override;
    virtual void stop() override;
    void joystickCallback(const sensor_msgs::Joy& msg);
};

}

bool QuadcopterControllerROS::configure(SimpleControllerConfig* config)
{
    if(!ros::isInitialized()){
        config->os() << config->controllerName()
                     << " cannnot be configured because ROS is not initialized." << std::endl;
        return false;
    }
    node.reset(new ros::NodeHandle);
    return true;
}

bool QuadcopterControllerROS::initialize(SimpleControllerIO* io)
{
    ioBody = io->body();
    os = &io->os();
    timeStep = io->timeStep();

    io->enableInput(ioBody->rootLink(), LINK_POSITION);

    cameraT = ioBody->link("CAMERA_T");
    cameraT->setActuationMode(Link::JOINT_TORQUE);
    io->enableIO(cameraT);
    qref = qprev = cameraT->q();

    for(int i = 0; i < 4; i++) {
        prop[i] = ioBody->link(propname[i]);
        prop[i]->setActuationMode(Link::JOINT_TORQUE);
        io->enableInput(prop[i], JOINT_VELOCITY);
        io->enableOutput(prop[i]);

        rotor[i] = ioBody->findDevice<Multicopter::RotorDevice>(rotorname[i]);
        io->enableInput(rotor[i]);
    }

    zrpyref = zrpyprev = getZRPY();
    dzrpyref = dzrpyprev = Vector4::Zero();

    isStableMode = false;
    prevModeButtonState = false;
    xyref = xyprev = getXY();
    dxyref = dxyprev = Vector2::Zero();

    power = powerprev = false;
    isStableMode = powerprev = false;

    //targetMode = joystick->addMode();
    rotorswitch = false;

    joystickSubscriber = node->subscribe("joy", 1, &QuadcopterControllerROS::joystickCallback, this);


    return true;
}


Vector4 QuadcopterControllerROS::getZRPY()
{
    auto T = ioBody->rootLink()->position();
    double z = T.translation().z();
    Vector3 rpy = rpyFromRot(T.rotation());
    return Vector4(z, rpy[0], rpy[1], rpy[2]);
}
    

Vector2 QuadcopterControllerROS::getXY()
{
    auto p = ioBody->rootLink()->translation();
    return Vector2(p.x(), p.y());
}

void QuadcopterControllerROS::joystickCallback(const sensor_msgs::Joy& msg)
{
    std::lock_guard<std::mutex> lock(joystickMutex);
    latestJoystickState = msg;
}

bool QuadcopterControllerROS::control()
{
    sensor_msgs::Joy joystick;
    {
        std::lock_guard<mutex> lock(joystickMutex);
        joystick = latestJoystickState;
    }
    joystick.axes.resize(Joystick::NUM_STD_AXES, 0.0f);
    joystick.buttons.resize(Joystick::NUM_STD_BUTTONS, 0);
    

    //control rotors
    Vector4 zrpy = getZRPY();
    double cc = cos(zrpy[1]) * cos(zrpy[2]);
    double gfcoef = 1.0 * 9.80665 / 4 / cc ;
    Vector4 force = Vector4::Zero();
    Vector4 torque = Vector4::Zero();
    
    ///power = joystick->getButtonState(targetMode, powerButton);
    power = joystick.buttons[POWER_BUTTON];
    if(power == false) {
        powerprev = false;
    } else if((power == true) && (powerprev != true)) {
        rotorswitch = !rotorswitch;
        powerprev = true;
    }

    ///bool modeButtonState = joystick->getButtonState(Joystick::R_STICK_BUTTON);
    bool modeButtonState = joystick.buttons[R_STICK_BUTTON];
    if(modeButtonState){
        if(!prevModeButtonState){
            isStableMode = !isStableMode;
        }
    }
    prevModeButtonState = modeButtonState;

    if(rotorswitch) {
        Vector4 f;

        Vector4 dzrpy = (zrpy - zrpyprev) / timeStep;
        Vector4 ddzrpy = (dzrpy - dzrpyprev) / timeStep;

        // For the stable mode
        Vector2 xy = getXY();
        Vector2 dxy = (xy - xyprev) / timeStep;
        Vector2 ddxy = (dxy - dxyprev) / timeStep;
        Vector2 dxy_local = Eigen::Rotation2Dd(-zrpy[3]) * dxy;
        Vector2 ddxy_local = Eigen::Rotation2Dd(-zrpy[3]) * ddxy;

        for(int axis = 0; axis < 4; ++axis) {
            double pos = -joystick.axes[rotorAxis[axis]];

            if((axis == 0) || (axis == 3)) {
                if(fabs(pos) > 0.25) {
                    dzrpyref[axis] = RATE[axis] * pos;
                } else {
                    dzrpyref[axis] = 0.0;
                }
                f[axis] = KP[axis] * (dzrpyref[axis] - dzrpy[axis]) + KD[axis] * (0.0 - ddzrpy[axis]);
            } else {
                if(!isStableMode){
                    ///if(fabs(pos) > 0.25) {
                    if(fabs(pos) > 0.15) {
                        zrpyref[axis] = RATE[axis] * pos;
                    } else {
                        zrpyref[axis] = 0.0;
                    }
                } else {
                    int axis_xy = axis - 1;
                    ///if(fabs(pos) > 0.25) {
                    if(fabs(pos) > 0.15) {
                        dxyref[axis_xy] = RATEX[axis_xy] * pos;
                    } else {
                        dxyref[axis_xy] = 0.0;
                    }
                    zrpyref[axis] =
                        KPX[axis_xy] * (dxyref[axis_xy] - dxy_local[1 - axis_xy]) +
                        KDX[axis_xy] * (0.0 - ddxy_local[1 - axis_xy]);

                    if(axis == 1) {
                        zrpyref[axis] *= -1.0;
                    }
                }
                f[axis] = KP[axis] * (zrpyref[axis] - zrpy[axis]) + KD[axis] * (0.0 - dzrpy[axis]);
            }

        }
        zrpyprev = zrpy;
        dzrpyprev = dzrpy;

        // For the stable mode
        xyprev = xy;
        dxyprev = dxy;
        
        for(int i = 0; i < 4; ++i) {
            double fi = 0.0;
            fi += gfcoef;
            fi += sign[i][0] * f[0];
            fi += sign[i][1] * f[1];
            fi += sign[i][2] * f[2];
            fi += sign[i][3] * f[3];
            force[i] = fi;
            torque[i] = dir[i] * fi;
        }
    }

    for(int i = 0; i < 4; ++i) {
        double tau = torque[i];
        rotor[i]->setTorque(tau);
        if(tau != 0.0) {
            prop[i]->u() = tau * 0.001;
        } else {
            double dq = prop[i]->dq();
            prop[i]->u() = 0.0005 * (0.0 - dq);
        }
        rotor[i]->setValue(force[i]);
        rotor[i]->notifyStateChange();
    }

    //control camera
    double q = cameraT->q();
    static const double P = 0.00002;
    static const double D = 0.00004;
    double dq = (q - qprev) / timeStep;
    double pos = joystick.axes[cameraAxis];
    double dqref = 0.0;
    if(fabs(pos) > 0.25) {
        double deltaq = 0.002 * pos;
        qref += deltaq;
        if(qref > 0) {
            qref = 0.0;
        } else if(qref < -M_PI) {
            qref = -M_PI;
        }
        dqref = deltaq / timeStep;
    }
    cameraT->u() = P * (qref - q) + D * (dqref - dq);
    qprev = q;

    return true;
}

void QuadcopterControllerROS::stop()
{
    joystickSubscriber.shutdown();
}

CNOID_IMPLEMENT_SIMPLE_CONTROLLER_FACTORY(QuadcopterControllerROS)
