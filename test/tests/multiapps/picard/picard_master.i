[Mesh]
  type = GeneratedMesh
  dim = 2
  nx = 5
  ny = 5
  parallel_type = replicated
  uniform_refine = 1
[]

[Variables]
  [./u]
  [../]
[]

[AuxVariables]
  [./v]
  [../]
[]

[Kernels]
  [./diff]
    type = CoefDiffusion
    variable = u
    coef = 0.1
  [../]
  [./time]
    type = TimeDerivative
    variable = u
  [../]
  [./force_u]
    type = CoupledForce
    variable = u
    v = v
  [../]
[]

[BCs]
  [./left]
    type = DirichletBC
    variable = u
    boundary = left
    value = 0
  [../]
  [./right]
    type = DirichletBC
    variable = u
    boundary = right
    value = 1
  [../]
[]

[Postprocessors]
  [picard_its]
    type = NumFixedPointIterations
    execute_on = 'initial timestep_end'
  [../]
[]

[Executioner]
  type = Transient
  num_steps = 20
  dt = 0.1
  solve_type = PJFNK
  petsc_options_iname = '-pc_type -pc_hypre_type'
  petsc_options_value = 'hypre boomeramg'
  fixed_point_max_its = 30
  nl_abs_tol = 1e-14
[]

[Outputs]
  exodus = true
[]

[MultiApps]
  [./sub]
    type = TransientMultiApp
    app_type = MooseTestApp
    positions = '0 0 0'
    input_files = picard_sub.i
    clone_master_mesh = true
  [../]
[]

[Transfers]
  [./v_from_sub]
    type = MultiAppNearestNodeTransfer
    from_multi_app = sub
    source_variable = v
    variable = v
  [../]
  [./u_to_sub]
    type = MultiAppNearestNodeTransfer
    to_multi_app = sub
    source_variable = u
    variable = u
  [../]
[]
