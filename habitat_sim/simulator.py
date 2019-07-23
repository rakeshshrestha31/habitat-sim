#!/usr/bin/env python3

# Copyright (c) Facebook, Inc. and its affiliates.
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import habitat_sim.bindings as hsim
from habitat_sim import utils
import habitat_sim.errors
from habitat_sim.agent import Agent, AgentState, AgentConfiguration
from typing import List
import numpy as np
import attr
from typing import List


@attr.s(auto_attribs=True, slots=True)
class Configuration(object):
    sim_cfg: hsim.SimulatorConfiguration = None
    agents: List[AgentConfiguration] = None


@attr.s
class Simulator:
    config: Configuration = attr.ib()
    agents: List = attr.ib(factory=list, init=False)
    _sim: hsim.SimulatorBackend = attr.ib(default=None, init=False)
    _num_total_frames: int = attr.ib(default=0, init=False)

    def __attrs_post_init__(self):
        config = self.config
        self.config = None
        self.reconfigure(config)

    def close(self):
        for agent in self.agents:
            agent.detach()

        self.agents = []
        self._sensors = None
        if self._sim is not None:
            del self._sim
            self._sim = None

    def seed(self, new_seed):
        self._sim.seed(new_seed)

    def reset(self):
        self._sim.reset()
        return self.get_sensor_observations()

    def _config_backend(self, config: Configuration):
        if self._sim is None:
            self._sim = hsim.SimulatorBackend(config.sim_cfg)
        else:
            self._sim.reconfigure(config.sim_cfg)

    def _config_agents(self, config: Configuration):
        if self.config is not None and self.config.agents == config.agents:
            return

        self.agents = [Agent(cfg) for cfg in config.agents]

    def reconfigure(self, config: Configuration):
        assert len(config.agents) > 0
        assert len(config.agents[0].sensor_specifications) > 0
        first_sensor_spec = config.agents[0].sensor_specifications[0]

        config.sim_cfg.height = first_sensor_spec.resolution[0]
        config.sim_cfg.width = first_sensor_spec.resolution[1]

        if self.config == config:
            return

        for agent in self.agents:
            agent.detach()

        # NB: Configure backend last as this gives more time for python's GC
        # to delete any previous instances of the simulator
        self._config_agents(config)
        self._config_backend(config)

        for i in range(len(self.agents)):
            self.agents[i].attach(
                self._sim.get_active_scene_graph().get_root_node().create_child()
            )
            self.agents[i].controls.move_filter_fn = self._step_filter

        self._default_agent = self.get_agent(config.sim_cfg.default_agent_id)

        agent_cfg = config.agents[config.sim_cfg.default_agent_id]
        self._sensors = {}
        for spec in agent_cfg.sensor_specifications:
            self._sensors[spec.uuid] = Sensor(
                sim=self._sim, agent=self._default_agent, sensor_id=spec.uuid
            )

        for i in range(len(self.agents)):
            self.initialize_agent(i)

        self.config = config

    def get_agent(self, agent_id):
        return self.agents[agent_id]

    def initialize_agent(self, agent_id, initial_state=None):
        agent = self.get_agent(agent_id=agent_id)
        if initial_state is None:
            initial_state = AgentState()
            initial_state.position = self._sim.pathfinder.get_random_navigable_point()
            initial_state.rotation = utils.quat_from_angle_axis(
                np.random.uniform(0, 2.0 * np.pi), np.array([0, 1, 0])
            )

        agent.set_state(initial_state)
        self._last_state = agent.state
        return agent

    def sample_random_agent_state(self, state_to_return):
        return self._sim.sample_random_agent_state(state_to_return)
    
    def get_scene_bounding_box_coords(self):
        return self._sim.get_scene_bounding_box_coords()

    def get_scene_point_cloud(self):
        return self._sim.get_scene_point_cloud()

    @property
    def pathfinder(self):
        return self._sim.pathfinder

    @property
    def semantic_scene(self):
        return self._sim.semantic_scene

    def get_sensor_observations(self):
        observations = {}
        for sensor_uuid, sensor in self._sensors.items():
            observations[sensor_uuid] = sensor.get_observation()
        return observations

    def last_state(self):
        return self._last_state

    def step(self, action):
        self._num_total_frames += 1
        self._default_agent.act(action)
        self._last_state = self._default_agent.get_state()
        observations = self.get_sensor_observations()
        return observations

    def make_action_pathfinder(self, agent_id=0):
        return self._sim.make_action_pathfinder(agent_id)

    def _step_filter(self, start_pos, end_pos):
        if self._sim.pathfinder.is_loaded:
            end_pos = self._sim.pathfinder.try_step(start_pos, end_pos)

        return end_pos

    def __del__(self):
        self.close()


class Sensor:
    r"""Wrapper around habitat_sim.Sensor

    TODO(MS) define entire Sensor class in python, reducing complexity
    """

    def __init__(self, sim, agent, sensor_id):
        self._sim = sim
        self._agent = agent

        # sensor is an attached object to the scene node
        # store such "attached object" in _sensor_object
        self._sensor_object = self._agent.sensors.get(sensor_id)

        self._spec = self._sensor_object.specification()
        if self._spec.sensor_type == hsim.SensorType.SEMANTIC:
            self._buffer = np.empty(
                (self._spec.resolution[0], self._spec.resolution[1]), dtype=np.uint32
            )
        elif self._spec.sensor_type == hsim.SensorType.DEPTH:
            self._buffer = np.empty(
                (self._spec.resolution[0], self._spec.resolution[1]), dtype=np.float32
            )
        else:
            self._buffer = np.empty(
                (
                    self._spec.resolution[0],
                    self._spec.resolution[1] * self._spec.channels,
                ),
                dtype=np.uint8,
            )

    def get_observation(self):
        # sanity check:
        # see if the sensor is attached to a scene graph, otherwise it is invalid,
        # and cannot make any observation
        if not self._sensor_object.is_valid:
            raise habitat_sim.errors.InvalidAttachedObject(
                "Sensor observation requested but sensor is invalid.\
                 (has it been detached from a scene node?)"
            )

        # get the correct scene graph based on application
        if self._spec.sensor_type == hsim.SensorType.SEMANTIC:
            if self._sim.semantic_scene is None:
                raise RuntimeError(
                    "SemanticSensor observation requested but no SemanticScene is loaded"
                )
            scene = self._sim.get_active_semantic_scene_graph()
        else:  # SensorType is DEPTH or any other type
            scene = self._sim.get_active_scene_graph()

        # now, connect the agent to the root node of the current scene graph

        # sanity check is not needed on agent:
        # because if a sensor is attached to a scene graph,
        # it implies the agent is attached to the same scene graph
        # (it assumes backend simulator will guarantee it.)

        agent_node = self._agent.scene_node
        agent_node.set_parent(scene.get_root_node())

        # draw the scene with the visual sensor:
        # it asserts the sensor is a visual sensor;
        # internally it will set the camera parameters (from the sensor) to the
        # default render camera in the scene so that
        # it has correct modelview matrix, projection matrix to render the scene
        self._sim.renderer.draw(self._sensor_object, scene)

        if self._spec.sensor_type == hsim.SensorType.SEMANTIC:
            self._sim.renderer.readFrameObjectId(self._buffer)
            return np.flip(self._buffer, axis=0).copy()
        elif self._spec.sensor_type == hsim.SensorType.DEPTH:
            self._sim.renderer.readFrameDepth(self._buffer)
            return np.flip(self._buffer, axis=0).copy()
        else:
            self._sim.renderer.readFrameRgba(self._buffer)
            return np.flip(
                self._buffer.reshape(
                    (
                        self._spec.resolution[0],
                        self._spec.resolution[1],
                        self._spec.channels,
                    )
                ),
                axis=0,
            ).copy()
