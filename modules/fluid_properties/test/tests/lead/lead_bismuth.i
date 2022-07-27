# Test LeadBismuthFluidProperties
#
# For temperature = 350K, the fluid properties should be:
# density =
# viscosity =
# cp =
# h =
# s =
# c =
# k =

[Mesh]
  type = GeneratedMesh
  dim = 2
  nx = 1
  ny = 1
[]

[Variables]
  [./dummy]
  [../]
[]

[AuxVariables]
  [./pressure]
    family = MONOMIAL
    order = CONSTANT
    initial_condition = 10.0e6
  [../]
  [./temperature]
    family = MONOMIAL
    order = CONSTANT
    initial_condition = 350
  [../]
  [./density]
    family = MONOMIAL
    order = CONSTANT
  [../]
  [./viscosity]
    family = MONOMIAL
    order = CONSTANT
  [../]
  [./cp]
    family = MONOMIAL
    order = CONSTANT
  [../]
  [./cv]
    family = MONOMIAL
    order = CONSTANT
  [../]
  [./internal_energy]
    family = MONOMIAL
    order = CONSTANT
  [../]
  [./enthalpy]
    family = MONOMIAL
    order = CONSTANT
  [../]
  [./entropy]
    family = MONOMIAL
    order = CONSTANT
  [../]
  [./thermal_cond]
    family = MONOMIAL
    order = CONSTANT
  [../]
  [./c]
    family = MONOMIAL
    order = CONSTANT
  [../]
[]

[AuxKernels]
  [./density]
    type = MaterialRealAux
     variable = density
     property = density
  [../]
  [./viscosity]
    type = MaterialRealAux
     variable = viscosity
     property = viscosity
  [../]
  [./cp]
    type = MaterialRealAux
     variable = cp
     property = cp
  [../]
  [./cv]
    type = MaterialRealAux
     variable = cv
     property = cv
  [../]
  [./e]
    type = MaterialRealAux
     variable = internal_energy
     property = e
  [../]
  [./enthalpy]
    type = MaterialRealAux
     variable = enthalpy
     property = h
  [../]
  [./entropy]
    type = MaterialRealAux
     variable = entropy
     property = s
  [../]
  [./thermal_cond]
    type = MaterialRealAux
     variable = thermal_cond
     property = k
  [../]
  [./c]
    type = MaterialRealAux
     variable = c
     property = c
  [../]
[]

[Modules]
  [./FluidProperties]
    [./lb]
      type = LeadBismuthFluidProperties
    [../]
  []
[]

[Materials]
  [./fp_mat]
    type = FluidPropertiesMaterialPT
    pressure = pressure
    temperature = temperature
    fp = lb
  [../]
[]

[Kernels]
  [./diff]
    type = Diffusion
    variable = dummy
  [../]
[]

[Executioner]
  type = Steady
  solve_type = NEWTON
[]

[Outputs]
  exodus = true
  csv = true
  file_base = lead_bismuth_out
[]
