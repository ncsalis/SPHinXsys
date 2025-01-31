/* -------------------------------------------------------------------------*
 *								SPHinXsys									*
 * -------------------------------------------------------------------------*
 * SPHinXsys (pronunciation: s'finksis) is an acronym from Smoothed Particle*
 * Hydrodynamics for industrial compleX systems. It provides C++ APIs for	*
 * physical accurate simulation and aims to model coupled industrial dynamic*
 * systems including fluid, solid, multi-body dynamics and beyond with SPH	*
 * (smoothed particle hydrodynamics), a meshless computational method using	*
 * particle discretization.													*
 *																			*
 * SPHinXsys is partially funded by German Research Foundation				*
 * (Deutsche Forschungsgemeinschaft) DFG HU1527/6-1, HU1527/10-1,			*
 *  HU1527/12-1 and HU1527/12-4													*
 *                                                                          *
 * Portions copyright (c) 2017-2022 Technical University of Munich and		*
 * the authors' affiliations.												*
 *                                                                          *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may  *
 * not use this file except in compliance with the License. You may obtain a*
 * copy of the License at http://www.apache.org/licenses/LICENSE-2.0.       *
 *                                                                          *
 * ------------------------------------------------------------------------*/
/**
 * @file 	fluid_boundary.h
 * @brief 	Here, we define the boundary condition classes for fluid dynamics.
 * @details The boundary conditions very often based on different types of buffers.
 * @author	Chi Zhang and Xiangyu Hu
 */

#ifndef FLUID_BOUNDARY_H
#define FLUID_BOUNDARY_H

#include "fluid_dynamics_inner.h"

#include "relax_dynamics.h"
#include <mutex>

namespace SPH
{
    namespace fluid_dynamics
    {
        /**
         * @class BaseFlowBoundaryCondition
         * @brief Base class for all boundary conditions.
         */
        class BaseFlowBoundaryCondition : public BaseLocalDynamics<BodyPartByCell>, public FluidDataSimple
        {
        public:
            BaseFlowBoundaryCondition(BodyPartByCell &body_part);
            virtual ~BaseFlowBoundaryCondition(){};

        protected:
            StdLargeVec<Real> &rho_, &p_;
            StdLargeVec<Vecd> &pos_, &vel_;
        };

        /**
         * @class FlowVelocityBuffer
         * @brief Flow buffer in which the particle velocity relaxes to a given target profile.
         * This technique will be used for applying several boundary conditions,
         * such as free stream, inflow, damping boundary conditions.
         */
        class FlowVelocityBuffer : public BaseFlowBoundaryCondition
        {
        public:
            FlowVelocityBuffer(BodyPartByCell &body_part, Real relaxation_rate = 0.3);
            virtual ~FlowVelocityBuffer(){};
            void update(size_t index_i, Real dt = 0.0);

        protected:
            /** default value is 0.3 suggests reaching target profile in several time steps */
            Real relaxation_rate_;

            /** Profile to be defined in applications,
             * argument parameters and return value are in frame (local) coordinate */
            virtual Vecd getTargetVelocity(Vecd &position, Vecd &velocity) = 0;
        };

        /**
         * @class   InflowVelocityCondition
         * @brief   Inflow boundary condition which imposes directly to a given velocity profile.
         *          TargetVelocity gives the velocity profile along the inflow direction,
         *          i.e. x direction in local frame.
         */
        template <typename TargetVelocity>
        class InflowVelocityCondition : public BaseFlowBoundaryCondition
        {
        public:
            /** default parameter indicates prescribe velocity */
            explicit InflowVelocityCondition(BodyAlignedBoxByCell &aligned_box_part, Real relaxation_rate = 1.0)
                : BaseFlowBoundaryCondition(aligned_box_part),
                  relaxation_rate_(relaxation_rate), aligned_box_(aligned_box_part.aligned_box_),
                  transform_(aligned_box_.getTransform()), halfsize_(aligned_box_.HalfSize()),
                  target_velocity(*this){};
            virtual ~InflowVelocityCondition(){};
            AlignedBoxShape &getAlignedBox() { return aligned_box_; };

            void update(size_t index_i, Real dt = 0.0)
            {
                Vecd frame_position = transform_.shiftBaseStationToFrame(pos_[index_i]);
                Vecd frame_velocity = transform_.xformBaseVecToFrame(vel_[index_i]);
                Vecd relaxed_frame_velocity = target_velocity(frame_position, frame_velocity) * relaxation_rate_ +
                                              frame_velocity * (1.0 - relaxation_rate_);
                vel_[index_i] = transform_.xformFrameVecToBase(relaxed_frame_velocity);
            };

        protected:
            Real relaxation_rate_;
            AlignedBoxShape &aligned_box_;
            Transformd &transform_;
            Vecd halfsize_;
            TargetVelocity target_velocity;
        };

        /**
         * @class   FreeStreamVelocityCorrection
         * @brief   modify the velocity of free surface particles with far-field velocity
         *          TargetVelocity gives the velocity profile along the free-stream direction,
         *          i.e. x direction in local frame.
         */
        template <typename TargetVelocity>
        class FreeStreamVelocityCorrection : public LocalDynamics, public FluidDataSimple
        {
        protected:
            Transformd transform_;
            Real rho0_;
            StdLargeVec<Real> &rho_sum_;
            StdLargeVec<Vecd> &pos_, &vel_;
            StdLargeVec<int> &surface_indicator_;
            TargetVelocity target_velocity;

        public:
            explicit FreeStreamVelocityCorrection(SPHBody &sph_body, const Transformd &transform = Transformd())
                : LocalDynamics(sph_body), FluidDataSimple(sph_body),
                  transform_(transform), rho0_(particles_->fluid_.ReferenceDensity()),
                  rho_sum_(particles_->rho_sum_), pos_(particles_->pos_), vel_(particles_->vel_),
                  surface_indicator_(*particles_->getVariableByName<int>("SurfaceIndicator")),
                  target_velocity(*this){};
            virtual ~FreeStreamVelocityCorrection(){};

            void update(size_t index_i, Real dt = 0.0)
            {
                if (surface_indicator_[index_i] == 1)
                {
                    Vecd frame_position = transform_.shiftBaseStationToFrame(pos_[index_i]);
                    Vecd frame_velocity = transform_.xformBaseVecToFrame(vel_[index_i]);
                    Real frame_u_stream_direction = frame_velocity[0];
                    Real u_freestream = target_velocity(frame_position, frame_velocity)[0];
                    frame_velocity[0] = u_freestream + (frame_u_stream_direction - u_freestream) *
                                                           SMIN(rho_sum_[index_i], rho0_) / rho0_;
                    vel_[index_i] = transform_.xformFrameVecToBase(frame_velocity);
                }
            };
        };

        /**
         * @class DampingBoundaryCondition
         * @brief damping boundary condition which relaxes
         * the particles to zero velocity profile.
         * TODO: one can using aligned box shape and generalize the damping factor along
         * one axis direction.
         */
        class DampingBoundaryCondition : public BaseFlowBoundaryCondition
        {
        public:
            explicit DampingBoundaryCondition(BodyRegionByCell &body_part);
            virtual ~DampingBoundaryCondition(){};
            void update(size_t index_particle_i, Real dt = 0.0);

        protected:
            /** default value is 0.1 suggests reaching  target inflow velocity in about 10 time steps */
            Real strength_;
            BoundingBox damping_zone_bounds_;
        };

        /**
         * @class EmitterInflowCondition
         * @brief Inflow boundary condition imposed on an emitter, in which pressure and density profile are imposed too.
         * The body part region is required to have parallel lower- and upper-bound surfaces.
         */
        class EmitterInflowCondition : public BaseLocalDynamics<BodyPartByParticle>, public FluidDataSimple
        {
        public:
            explicit EmitterInflowCondition(BodyAlignedBoxByParticle &aligned_box_part);
            virtual ~EmitterInflowCondition(){};

            virtual void setupDynamics(Real dt = 0.0) override { updateTransform(); };
            void update(size_t unsorted_index_i, Real dt = 0.0);

        protected:
            Fluid &fluid_;
            StdLargeVec<Vecd> &pos_, &vel_, &acc_;
            StdLargeVec<Real> &rho_, &p_, &drho_dt_;
            /** inflow pressure condition */
            Real inflow_pressure_;
            Real rho0_;
            AlignedBoxShape &aligned_box_;
            Transformd &updated_transform_, old_transform_;

            /** no transform by default */
            virtual void updateTransform(){};
            virtual Vecd getTargetVelocity(Vecd &position, Vecd &velocity) = 0;
        };

        /**
         * @class EmitterInflowInjection
         * @brief Inject particles into the computational domain.
         * Note that the axis is at the local coordinate and upper bound direction is
         * the local positive direction.
         */
        class EmitterInflowInjection : public BaseLocalDynamics<BodyPartByParticle>, public FluidDataSimple
        {
        public:
            EmitterInflowInjection(BodyAlignedBoxByParticle &aligned_box_part,
                                   size_t body_buffer_width, int axis);
            virtual ~EmitterInflowInjection(){};

            void update(size_t unsorted_index_i, Real dt = 0.0);

        protected:
            std::mutex mutex_switch_to_real_; /**< mutex exclusion for memory conflict */
            Fluid &fluid_;
            StdLargeVec<Vecd> &pos_;
            StdLargeVec<Real> &rho_, &p_;
            const int axis_; /**< the axis direction for bounding*/
            AlignedBoxShape &aligned_box_;
        };

        /**
         * @class DisposerOutflowDeletion
         * @brief Delete particles who ruing out the computational domain.
         */
        class DisposerOutflowDeletion : public BaseLocalDynamics<BodyPartByCell>, public FluidDataSimple
        {
        public:
            DisposerOutflowDeletion(BodyAlignedBoxByCell &aligned_box_part, int axis);
            virtual ~DisposerOutflowDeletion(){};

            void update(size_t index_i, Real dt = 0.0);

        protected:
            std::mutex mutex_switch_to_buffer_; /**< mutex exclusion for memory conflict */
            StdLargeVec<Vecd> &pos_;
            const int axis_; /**< the axis direction for bounding*/
            AlignedBoxShape &aligned_box_;
        };

        /**
         * @class StaticConfinementDensity
         * @brief static confinement condition for density summation
         */
        class StaticConfinementDensity : public BaseLocalDynamics<BodyPartByCell>, public FluidDataSimple
        {
        public:
            StaticConfinementDensity(NearShapeSurface &near_surface);
            virtual ~StaticConfinementDensity(){};
            void update(size_t index_i, Real dt = 0.0);

        protected:
            Real rho0_, inv_sigma0_;
            StdLargeVec<Real> &mass_, &rho_sum_;
            StdLargeVec<Vecd> &pos_;
            LevelSetShape *level_set_shape_;
        };

        /**
         * @class StaticConfinementIntegration1stHalf
         * @brief static confinement condition for pressure relaxation
         */
        class StaticConfinementIntegration1stHalf : public BaseLocalDynamics<BodyPartByCell>, public FluidDataSimple
        {
        public:
            StaticConfinementIntegration1stHalf(NearShapeSurface &near_surface);
            virtual ~StaticConfinementIntegration1stHalf(){};
            void update(size_t index_i, Real dt = 0.0);

        protected:
            Fluid &fluid_;
            StdLargeVec<Real> &rho_, &p_;
            StdLargeVec<Vecd> &pos_, &vel_, &acc_;
            LevelSetShape *level_set_shape_;
            AcousticRiemannSolver riemann_solver_;
        };

        /**
         * @class StaticConfinementIntegration2ndHalf
         * @brief static confinement condition for density relaxation
         */
        class StaticConfinementIntegration2ndHalf : public BaseLocalDynamics<BodyPartByCell>, public FluidDataSimple
        {
        public:
            StaticConfinementIntegration2ndHalf(NearShapeSurface &near_surface);
            virtual ~StaticConfinementIntegration2ndHalf(){};
            void update(size_t index_i, Real dt = 0.0);

        protected:
            Fluid &fluid_;
            StdLargeVec<Real> &rho_, &p_, &drho_dt_;
            StdLargeVec<Vecd> &pos_, &vel_;
            LevelSetShape *level_set_shape_;
            AcousticRiemannSolver riemann_solver_;
        };

        /**
         * @class StaticConfinement
         * @brief Static confined boundary condition for complex structures.
         */
        class StaticConfinement
        {
        public:
            SimpleDynamics<StaticConfinementDensity> density_summation_;
            SimpleDynamics<StaticConfinementIntegration1stHalf> pressure_relaxation_;
            SimpleDynamics<StaticConfinementIntegration2ndHalf> density_relaxation_;
           SimpleDynamics<relax_dynamics::ShapeSurfaceBounding> surface_bounding_;

            StaticConfinement(NearShapeSurface &near_surface);
            virtual ~StaticConfinement(){};
        };

    }
}
#endif // FLUID_BOUNDARY_H