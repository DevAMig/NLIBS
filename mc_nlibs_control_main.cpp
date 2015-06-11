/****************************************************************************
 *
 *   Copyright (c) 2015 Elikos Team. All rights reserved.
 *   Authors: @author Antoine Mignon <mignon.antoine@gmail.com>
 *			  @author Alexandre Borowczyk <borowczyk.alexandre@gmail.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file mc_nlibs_control_main.cpp
 * Multicopter non linear integral backstepping controller.
 *
 * Original publication for the designed controller:
 * --
 * --
 *
 * Desciption of the controller
 * --
 * --
 *
 * @author Antoine Mignon <mignon.antoine@gmail.com>
 * @author Alexandre Borowczyk <borowczyk.alexandre@gmail.com>
 */

#include <px4.h>
#include <nuttx/config.h>
#include <functional>
#include <stdio.h>
#include <cstdio>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <drivers/drv_hrt.h>
#include <arch/board/board.h>

#include <uORB/uORB.h>
#include <uORB/topics/vehicle_attitude_setpoint.h>
#include <uORB/topics/manual_control_setpoint.h>
#include <uORB/topics/actuator_controls.h>
#include <uORB/topics/actuator_controls_virtual_fw.h>
#include <uORB/topics/actuator_controls_virtual_mc.h>
#include <uORB/topics/vehicle_rates_setpoint.h>
#include <uORB/topics/fw_virtual_rates_setpoint.h>
#include <uORB/topics/mc_virtual_rates_setpoint.h>
#include <uORB/topics/vehicle_attitude.h>
#include <uORB/topics/vehicle_control_mode.h>
#include <uORB/topics/actuator_armed.h>
#include <uORB/topics/vehicle_status.h>
#include <uORB/topics/parameter_update.h>
#include <uORB/topics/vehicle_local_position.h>
#include <uORB/topics/position_setpoint_triplet.h>
#include <uORB/topics/vehicle_global_velocity_setpoint.h>
#include <uORB/topics/vehicle_local_position_setpoint.h>
#include <uORB/topics/multirotor_motor_limits.h>
#include <uORB/topics/mc_att_ctrl_status.h>

#include <systemlib/param/param.h>
#include <systemlib/err.h>
#include <systemlib/perf_counter.h>
#include <systemlib/circuit_breaker.h>
#include <systemlib/systemlib.h>
#include <mathlib/mathlib.h>
#include <lib/geo/geo.h>
#include <mavlink/mavlink_log.h>
#include <platforms/px4_defines.h>

#define TILT_COS_MAX			0.7f
#define SIGMA					0.000001f
#define MIN_DIST				0.01f

#define YAW_DEADZONE			0.05f
#define MIN_TAKEOFF_THRUST   	0.2f
#define RATES_I_LIMIT			0.3f

 /**
 * Multicopter NLIBS control app start / stop handling function
 *
 * @ingroup apps
 */
extern "C" __EXPORT int mn_nlibs_control_main(int argc, char *argv[]);

class MulticopterNLIBSControl
{
public:
	/**
	 * Constructor
	 */
	MulticopterNLIBSControl();

	/**
	 * Destructor, also kills task.
	 */
	~MulticopterNLIBSControl();

	/**
	 * Start task.
	 *
	 * @return		OK on success.
	 */
	int		start();

private:
	const float alt_ctl_dz = 0.2f;

	bool	_task_should_exit;		/**< if true, task should exit */
	int		_control_task;			/**< task handle for task */
	int		_mavlink_fd;			/**< mavlink fd */

	int		_att_sub;				/**< vehicle attitude subscription */
	int		_att_sp_sub;			/**< vehicle attitude setpoint */
	int		_control_mode_sub;		/**< vehicle control mode subscription */
	int		_params_sub;			/**< notification of parameter updates */
	int		_manual_sub;			/**< notification of manual control updates */
	int		_arming_sub;			/**< arming status of outputs */
	int		_local_pos_sub;			/**< vehicle local position */
	int		_pos_sp_triplet_sub;	/**< position setpoint triplet */
	int		_local_pos_sp_sub;		/**< offboard local position setpoint */
	int		_global_vel_sp_sub;		/**< offboard global velocity setpoint */

	orb_advert_t	_att_sp_pub;			/**< attitude setpoint publication */
	orb_advert_t	_local_pos_sp_pub;		/**< vehicle local position setpoint publication */
	orb_advert_t	_global_vel_sp_pub;		/**< vehicle global velocity setpoint publication */
	orb_advert_t	_controller_status_pub;	/**< controller status publication */
	orb_advert_t	_v_rates_sp_pub;		/**< rate setpoint publication */
	orb_advert_t	_actuators_0_pub;		/**< attitude actuator controls publication */

	bool	_actuators_0_circuit_breaker_enabled;	/**< circuit breaker to suppress output */

	struct vehicle_attitude_s					_att;				/**< vehicle attitude */
	struct vehicle_attitude_setpoint_s			_att_sp;			/**< vehicle attitude setpoint */
	struct vehicle_rates_setpoint_s				_rates_sp;		/**< vehicle rates setpoint */
	struct manual_control_setpoint_s			_manual;			/**< r/c channel data */
	struct vehicle_control_mode_s				_control_mode;		/**< vehicle control mode */
	struct actuator_armed_s						_arming;			/**< actuator arming status */
	struct actuator_controls_s					_actuators;			/**< actuator controls */
	struct vehicle_status_s						_vehicle_status;	/**< vehicle status */
	struct multirotor_motor_limits_s			_motor_limits;		/**< motor limits */
	struct mc_att_ctrl_status_s 				_controller_status; /**< controller status */
	struct vehicle_local_position_s				_local_pos;			/**< vehicle local position */
	struct position_setpoint_triplet_s			_pos_sp_triplet;	/**< vehicle global position setpoint triplet */
	struct vehicle_local_position_setpoint_s	_local_pos_sp;		/**< vehicle local position setpoint */
	struct vehicle_global_velocity_setpoint_s	_global_vel_sp;		/**< vehicle global velocity setpoint */

	struct {
		param_t q_mass;
		param_t q_ix_moment;
		param_t q_iy_moment;
		param_t q_iz_moment;
		param_t q_arm_length;
		param_t q_drag_coeff;
		param_t q_xlin_drag;
		param_t q_ylin_drag;
		param_t q_zlin_drag;
		param_t q_xrot_drag;
		param_t q_yrot_drag;
		param_t q_zrot_drag;
		param_t q_rotor_radius;
		param_t q_rotor_twist_angle;
		param_t q_rotor _root_angle;
		param_t q_motor_cst;

		param_t thr_min;
		param_t thr_max;
		param_t x_gain;
		param_t y_gain;
		param_t x_vel_gain;
		param_t y_vel_gain;
		param_t phi_gain;
		param_t theta_gain;
		param_t phi_vel_gain;
		param_t theta_vel_gain;
		param_t psi_gain;
		param_t z_gain;
		param_t psi_vel_gain;
		param_t z_vel_gain;
		param_t f1_gain;
		param_t f2_gain;
		param_t f3_gain;
		param_t f4_gain;

		param_t xy_vel_max;
		param_t xy_ff;
		param_t tilt_max_air;
		param_t tilt_max_land;
		param_t land_speed;
		param_t man_roll_max;
		param_t man_pitch_max;
		param_t man_yaw_max;

	} _params_handles;		/**< handles for interesting parameters */

	struct {
		float q_mass;
		float q_ix_moment;
		float q_iy_moment;
		float q_iz_moment;
		float q_arm_length;
		float q_drag_coeff;
		float q_xlin_drag;
		float q_ylin_drag;
		float q_zlin_drag;
		float q_xrot_drag;
		float q_yrot_drag;
		float q_zrot_drag;
		float q_rotor_radius;
		float q_rotor_twist_angle;
		float q_rotor _root_angle;
		float q_motor_cst;

		float thr_min;
		float thr_max;
		float tilt_max_air;
		float land_speed;
		float tilt_max_land;
		float man_roll_max;
		float man_pitch_max;
		float man_yaw_max;
		float yaw_ff;
		float roll_rate_max;
		float pitch_rate_max;
		float yaw_rate_max;

		math::Vector<3> nlibs_rate_max;

		math::Matrix<2, 2> A1_gain;
		math::Matrix<2, 2> A2_gain;
		math::Matrix<2, 2> A3_gain;
		math::Matrix<2, 2> A4_gain;
		math::Matrix<2, 2> A5_gain;
		math::Matrix<2, 2> A6_gain;
		math::Matrix<4, 4> A7_gain;

	} _params;

	struct map_projection_reference_s _ref_pos;
	float _ref_alt;
	hrt_abstime _ref_timestamp;

	bool _reset_pos_sp;
	bool _reset_alt_sp;
	bool _reset_att_sp;
	bool _reset_yaw_s;
	bool _mode_auto;

	math::Vector<3> _pos;
	math::Vector<3> _pos_sp;
	math::Vector<3> _vel;
	math::Vector<3> _vel_sp;
	math::Vector<3> _vel_prev;			/**< velocity on previous step */
	math::Vector<3> _vel_ff;
	math::Vector<3> _sp_move_rate;
	math::Vector<3>	_rates_prev;		/**< angular rates on previous step */
	math::Vector<3>	_rates_sp;			/**< angular rates setpoint */
	math::Vector<3>	_rates_int;			/**< angular rates integral error */
	math::Vector<3>	_att_control;		/**< attitude control vector */

	math::Matrix<3, 3>  _I;				/**< identity matrix */

	float	_thrust_sp;					/**< thrust setpoint */

	/**
	 * Update our local parameter cache.
	 */
	int	parameters_update(bool force);

	/**
	 * Update control outputs
	 */
	void control_update();

	/**
	 * Check for changes in subscribed topics.
	 */
	void poll_subscriptions();

	static float	scale_control(float ctl, float end, float dz);

	/**
	 * Update reference for local position projection
	 */
	void		update_ref();
	/**
	 * Reset position setpoint to current position
	 */
	void		reset_pos_sp();

	/**
	 * Reset altitude setpoint to current altitude
	 */
	void		reset_alt_sp();

	/**
	 * Check if position setpoint is too far from current position and adjust it if needed.
	 */
	void		limit_pos_sp_offset();

	/**
	 * Set position setpoint using manual control
	 */
	void		control_manual(float dt);

	/**
	 * Set position setpoint using offboard control
	 */
	void		control_offboard(float dt);

	bool		cross_sphere_line(const math::Vector<3>& sphere_c, float sphere_r,
					const math::Vector<3> line_a, const math::Vector<3> line_b, math::Vector<3>& res);

	/**
	 * Set position setpoint for AUTO
	 */
	void		control_auto(float dt);

	/**
	 * Select between barometric and global (AMSL) altitudes
	 */
	void		select_alt(bool global);

	/**
	 * Shim for calling task_main from task_create.
	 */
	static void	task_main_trampoline(int argc, char *argv[]);

	/**
	 * Main sensor collection task.
	 */
	void		task_main();
};

namespace nlibs_control
{

/* oddly, ERROR is not defined for c++ */
#ifdef ERROR
# undef ERROR
#endif
static const int ERROR = -1;

MulticopterNLIBSControl	*g_control;
}

MulticopterNLIBSControl::MulticopterNLIBSControl() :

	b_task_should_exit(false),
	_control_task(-1),
	_mavlink_fd(-1),

	/* subscriptions */
	_att_sub(-1),
	_att_sp_sub(-1),
	_control_mode_sub(-1),
	_params_sub(-1),
	_manual_sub(-1)(-1),
	_arming_sub(-1),
	_local_pos_sub(-1),
	_pos_sp_triplet_sub(-1),
	_local_pos_sp_sub(-1),
	_global_vel_sp_sub(-1),

	/* publications */
	_att_sp_pub(-1),
	_local_pos_sp_pub(-1),
	_global_vel_sp_pub(-1),
	_controller_status_pub(-1),
	_v_rates_sp_pub(-1),
	_actuators_0_pub(-1),

	_actuators_0_circuit_breaker_enabled(false),

{
	memset(&_att, 0, sizeof(_att));
	memset(&_att_sp, 0, sizeof(_att_sp));
	memset(&_rates_sp, 0, sizeof(_rates_sp));
	memset(&_manual, 0, sizeof(_manual));
	memset(&_control_mode, 0, sizeof(_control_mode));
	memset(&_arming, 0, sizeof(_arming));
	memset(&_actuators, 0, sizeof(_actuators));
	memset(&_vehicle_status, 0, sizeof(_vehicle_status));
	memset(&_motor_limits, 0, sizeof(_motor_limits));
	memset(&_controller_status, 0, sizeof(_controller_status));
	memset(&_local_pos, 0, sizeof(_local_pos));
	memset(&_pos_sp_triplet, 0, sizeof(_pos_sp_triplet));
	memset(&_local_pos_sp, 0, sizeof(_local_pos_sp));
	memset(&_global_vel_sp, 0, sizeof(_global_vel_sp));

	memset(&_ref_pos, 0, sizeof(_ref_pos));

	_params.nlibs_rate_max.zero();

	_params.A1_gain.zero();
	_params.A2_gain.zero();
	_params.A3_gain.zero();
	_params.A4_gain.zero();
	_params.A5_gain.zero();
	_params.A6_gain.zero();
	_params.A7_gain.zero();

	_pos.zero();
	_pos_sp.zero();
	_vel.zero();
	_vel_sp.zero();
	_vel_prev.zero();
	_vel_ff.zero();
	_sp_move_rate.zero();
	_rates_prev.zero();
	_rates_sp.zero();
	_rates_int.zero();
	_att_control.zero();

	_params_handles.q_mass				= param_find("NLIBSC_QMASS");
	_params_handles.q_ix_moment			= param_find("NLIBSC_QIX_MOMENT");
	_params_handles.q_iy_moment			= param_find("NLIBSC_QIY_MOMENT");
	_params_handles.q_iz_moment			= param_find("NLIBSC_QIZ_MOMENT");
	_params_handles.q_arm_length		= param_find("NLIBSC_QARM_LENGTH");
	_params_handles.q_drag_coeff		= param_find("NLIBSC_QDRAG_COEFF");
	_params_handles.q_xlin_drag			= param_find("NLIBSC_QXLIN_DRAG");
	_params_handles.q_ylin_drag			= param_find("NLIBSC_QYLIN_DRAG");
	_params_handles.q_zlin_drag			= param_find("NLIBSC_QZLIN_DRAG");
	_params_handles.q_xrot_drag			= param_find("NLIBSC_QXROT_DRAG");
	_params_handles.q_yrot_drag			= param_find("NLIBSC_QYROT_DRAG");
	_params_handles.q_zrot_drag			= param_find("NLIBSC_QZROT_DRAG");
	_params_handles.q_rotor_radius		= param_find("NLIBSC_QROTOR_RADIUS");
	_params_handles.q_rotor_twist_angle	= param_find("NLIBSC_QROTOR_TWIST_ANGLE");
	_params_handles.q_rotor _root_angle	= param_find("NLIBSC_QROTOR_ROOT_ANGLE");
	_params_handles.q_motor_cst			= param_find("NLIBSC_QMOTOR_CST");

	_params_handles.thr_min				= param_find("NLIBSC_THR_MIN");
	_params_handles.thr_max				= param_find("NLIBSC_THR_MAX");
	_params_handles.x_gain				= param_find("NLIBSC_X_GAIN");
	_params_handles.y_gain				= param_find("NLIBSC_Y_GAIN");
	_params_handles.x_vel_gain			= param_find("NLIBSC_X_VEL_GAIN");
	_params_handles.y_vel_gain			= param_find("NLIBSC_Y_VEL_GAIN");
	_params_handles.phi_gain			= param_find("NLIBSC_PHI_GAIN");
	_params_handles.theta_gain			= param_find("NLIBSC_THETA_GAIN");
	_params_handles.phi_vel_gain		= param_find("NLIBSC_PHI_RATE_GAIN");
	_params_handles.theta_vel_gain		= param_find("NLIBSC_THETA_RATE_GAIN");
	_params_handles.psi_gain			= param_find("NLIBSC_PSI_GAIN");
	_params_handles.z_gain				= param_find("NLIBSC_Z_GAIN");
	_params_handles.psi_vel_gain		= param_find("NLIBSC_PSI_RATE_GAIN");
	_params_handles.z_vel_gain			= param_find("NLIBSC_Z_VEL_GAIN");
	_params_handles.f1_gain				= param_find("NLIBSC_F1_GAIN");
	_params_handles.f2_gain				= param_find("NLIBSC_F2_GAIN");
	_params_handles.f3_gain				= param_find("NLIBSC_F3_GAIN");
	_params_handles.f4_gain				= param_find("NLIBSC_F4_GAIN");

	_params_handles.xy_vel_max			= param_find("NLIBSC_ROLL_RATE_MAX");
	_params_handles.xy_ff				= param_find("NLIBSC_PITCH_RATE_MAX");
	_params_handles.tilt_max_air		= param_find("NLIBSC_YAW_RATE_MAX");
	_params_handles.tilt_max_land		= param_find("NLIBSC_XY_VEL_MAX");
	_params_handles.land_speed			= param_find("NLIBSC_XY_FF");
	_params_handles.man_roll_max		= param_find("NLIBSC_TILTMAX_AIR");
	_params_handles.man_pitch_max		= param_find("NLIBSC_TILTMAX_LND");
	_params_handles.man_yaw_max			= param_find("NLIBSC_LAND_SPEED");

	/* fetch initial parameter values */
	parameters_update();
}

MulticopterNLIBSControl::~MulticopterNLIBSControl()
{
	if (_control_task != -1)
		/* task wakes up every 100ms or so at the longest */
		_task_should_exit = true;

		/* wait for a second for the task to quit at our request */
		unsigned i = 0;

		do {
			/* wait 20ms */
			usleep(20000);

			/* if we have given up, kill it */
			if (++i > 50) {
				task_delete(_control_task);
				break;
			}
		} while (_control_task != -1);
	}

	pos_control::g_control = nullptr;
}

void MulticopterNLIBSControl::parameter_update()
{
	float v;
/*
		float q_rotor_radius;
		float q_rotor_twist_angle;
		float q_rotor _root_angle;
		float q_motor_cst;

		float thr_min;
		float thr_max;
		float tilt_max_air;
		float land_speed;
		float tilt_max_land;
		float man_roll_max;
		float man_pitch_max;
		float man_yaw_max;
		float yaw_ff;
		float roll_rate_max;
		float pitch_rate_max;
		float yaw_rate_max;

		math::Vector<3> nlibs_rate_max; */

	/* Quadrotor parameters */
	param_get(_params_handles.q_mass, &_params.q_mass);
	param_get(_params_handles.q_ix_moment, &_params.q_ix_moment);
	param_get(_params_handles.q_iy_moment, &_params.q_iy_moment);
	param_get(_params_handles.q_iz_moment, &_params.q_iz_moment);
	param_get(_params_handles.q_arm_length, &_params.q_arm_length);
	param_get(_params_handles.q_drag_coeff, &_params.q_drag_coeff);
	param_get(_params_handles.q_xlin_drag, &_params.q_xlin_drag);
	param_get(_params_handles.q_ylin_drag, &_params.q_ylin_drag);
	param_get(_params_handles.q_zlin_drag, &_params.q_zlin_drag);
	param_get(_params_handles.q_xrot_drag, &_params.q_xrot_drag);
	param_get(_params_handles.q_yrot_drag, &_params.q_yrot_drag);
	param_get(_params_handles.q_zrot_drag, &_params.q_zrot_drag);







	/* A1 gains */
	param_get(_params_handles.x_gain, &v);
	_params.A1_gain(0,0) = v;

	param_get(_params_handles.y_gain, &v);
	_params.A1_gain(1,1) = v;

	/* A2 gains */
	param_get(_params_handles.x_vel_gain, &v);
	_params.A2_gain(0,0) = v;

	param_get(_params_handles.y_vel_gain, &v);
	_params.A2_gain(1,1) = v;

	/* A3 gains */
	param_get(_params_handles.phi_gain, &v);
	_params.A3_gain(0,0) = v;

	param_get(_params_handles.theta_gain, &v);
	_params.A3_gain(1,1) = v;

	/* A4 gains */
	param_get(_param_handles.phi_vel_gain, &v);
	_params.A4_gain(0,0) = v;

	param_get(_params_handles.theta_vel_gain, &v);
	_params.A4_gain(1,1) = v;

	/* A5 gains */
	param_get(_params_handles.psi_gain, &v);
	_params.A5_gain(0,0) = v;

	param_get(_params_handles.z_gain, &v);
	_params.A5_gain(1,1) = v;

	/* A6 gains */
	param_get(_params_handles.psi_vel_gain, &v);
	_params.A6_gain(0,0) = v;

	param_get(_params_handles.z_vel_gain, &v);
	_params.A6_gain(1,1) = v;

	/* A7 gains */
	param_get(_params_handles.f1_gain, &v);
	_params.A7_gain(0,0) = v;

	param_get(_params_handles.f2_gain, &v);
	_params.A7_gain(1,1) = v;

	param_get(_params_handles.f3_gain, &v);
	_params.A7_gain(2,2) = v;

	param_get(_params_handles.f4_gain, &v);
	_params.A7_gain(3,3) = v;


}