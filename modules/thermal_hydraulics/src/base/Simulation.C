//* This file is part of the MOOSE framework
//* https://www.mooseframework.org
//*
//* All rights reserved, see COPYRIGHT for full restrictions
//* https://github.com/idaholab/moose/blob/master/COPYRIGHT
//*
//* Licensed under LGPL 2.1, please see LICENSE for details
//* https://www.gnu.org/licenses/lgpl-2.1.html

#include "Simulation.h"
#include "FEProblemBase.h"
#include "AddVariableAction.h"
#include "MooseObjectAction.h"
#include "Transient.h"
#include "HeatConductionModel.h"
#include "HeatStructureBase.h"
#include "FlowChannelBase.h"
#include "FlowJunction.h"

#include "ClosuresBase.h"
#include "FluidProperties.h"
#include "THMControl.h"
#include "TerminateControl.h"
#include "THMMesh.h"
#include "RelationshipManager.h"
#include "NonlinearSystemBase.h"
#include "TimeIntegrator.h"
#include "ExplicitTimeIntegrator.h"
#include "ExplicitEuler.h"
#include "ExplicitRK2.h"
#include "ExplicitTVDRK2.h"

#include "libmesh/string_to_enum.h"

Simulation::Simulation(FEProblemBase & fe_problem, const InputParameters & pars)
  : ParallelObject(fe_problem.comm()),
    LoggingInterface(_log),
    _mesh(*static_cast<THMMesh *>(pars.get<MooseMesh *>("mesh"))),
    _fe_problem(fe_problem),
    _app(static_cast<ThermalHydraulicsApp &>(*pars.get<MooseApp *>("_moose_app"))),
    _factory(_app.getFactory()),
    _pars(pars),
    _flow_fe_type(FEType(CONSTANT, MONOMIAL)),
    _implicit_time_integration(true),
    _check_jacobian(false),
    _output_vector_velocity(true),
    _zero(0)
{
  bool second_order_mesh = pars.get<bool>("2nd_order_mesh");
  HeatConductionModel::_fe_type =
      second_order_mesh ? FEType(SECOND, LAGRANGE) : FEType(FIRST, LAGRANGE);

  if (Moose::_warnings_are_errors)
    _log.setWarningsAsErrors();
}

Simulation::~Simulation()
{
  for (auto && k : _control_data)
    delete k.second;
}

void
Simulation::augmentSparsity(const dof_id_type & elem_id1, const dof_id_type & elem_id2)
{
  auto it = _sparsity_elem_augmentation.find(elem_id1);
  if (it == _sparsity_elem_augmentation.end())
    it = _sparsity_elem_augmentation.insert(_sparsity_elem_augmentation.begin(),
                                            {elem_id1, std::vector<dof_id_type>()});
  it->second.push_back(elem_id2);

  it = _sparsity_elem_augmentation.find(elem_id2);
  if (it == _sparsity_elem_augmentation.end())
    it = _sparsity_elem_augmentation.insert(_sparsity_elem_augmentation.begin(),
                                            {elem_id2, std::vector<dof_id_type>()});
  it->second.push_back(elem_id1);
}

void
Simulation::buildMesh()
{
  if (_components.size() == 0)
    return;

  // perform any pre-mesh-setup initialization
  for (auto && comp : _components)
    comp->executePreSetupMesh();

  // build mesh
  for (auto && comp : _components)
    comp->executeSetupMesh();
  // Make sure all node sets have their corresponding side sets
  if (_mesh.getMesh().get_boundary_info().n_nodeset_conds() > 0)
    _mesh.getMesh().get_boundary_info().build_side_list_from_node_list();
}

void
Simulation::setupQuadrature()
{
  if (_components.size() == 0)
    return;

  Order order = CONSTANT;
  unsigned int n_flow_channels = 0;
  unsigned int n_heat_structures = 0;

  for (auto && comp : _components)
  {
    auto flow_channel = dynamic_cast<FlowChannelBase *>(comp.get());
    if (flow_channel != nullptr)
      n_flow_channels++;

    auto heat_structure = dynamic_cast<HeatStructureBase *>(comp.get());
    if (heat_structure != nullptr)
      n_heat_structures++;
  }

  if (n_flow_channels > 0)
  {
    const FEType & fe_type = getFlowFEType();
    if (fe_type.default_quadrature_order() > order)
      order = fe_type.default_quadrature_order();
  }
  if (n_heat_structures > 0)
  {
    const FEType & fe_type = HeatConductionModel::feType();
    if (fe_type.default_quadrature_order() > order)
      order = fe_type.default_quadrature_order();
  }

  _fe_problem.createQRules(QGAUSS, order, order, order);
}

void
Simulation::initSimulation()
{
  // sort the components using dependency resolver
  DependencyResolver<std::shared_ptr<Component>> dependency_resolver;
  for (const auto & comp : _components)
  {
    dependency_resolver.addItem(comp);
    for (const auto & dep : comp->getDependencies())
    {
      if (hasComponent(dep))
        dependency_resolver.insertDependency(comp, _comp_by_name[dep]);
    }
  }

  std::sort(_components.begin(), _components.end(), dependency_resolver);
}

void
Simulation::initComponents()
{
  // initialize components
  for (auto && comp : _components)
    comp->executeInit();

  // perform secondary initialization, which relies on init() being called
  // already for all components
  for (auto && comp : _components)
    comp->executeInitSecondary();
}

void
Simulation::identifyLoops()
{
  // loop over junctions and boundaries (non-geometrical components)
  for (const auto & component : _components)
  {
    const auto flow_connection =
        MooseSharedNamespace::dynamic_pointer_cast<FlowConnection>(component);
    if (flow_connection)
    {
      // create vector of names of this component and its connected flow channels, and then sort
      // them
      std::vector<std::string> names = flow_connection->getConnectedComponentNames();
      names.push_back(component->name());
      std::sort(names.begin(), names.end());

      // pick first name alphabetically to be the proposed loop name
      std::string proposed_loop_name = names[0];

      for (const std::string & name : names)
      {
        // if the name is not yet in the map
        if (_component_name_to_loop_name.find(name) == _component_name_to_loop_name.end())
          // just add the new map key; nothing else needs updating
          _component_name_to_loop_name[name] = proposed_loop_name;
        else
        {
          // compare to the existing loop name for this component to make sure the
          // proposed loop name is first alphabetically
          const std::string current_loop_name = _component_name_to_loop_name[name];
          // if proposed loop name comes later, change map values of the current
          // loop name to be the proposed name, and then update the proposed name
          // to be the current name
          if (proposed_loop_name > current_loop_name)
          {
            for (auto && entry : _component_name_to_loop_name)
              if (entry.second == proposed_loop_name)
                entry.second = current_loop_name;
            proposed_loop_name = current_loop_name;
          }
          // if proposed loop name comes earlier, change map values of the current
          // loop name to be the proposed name
          else if (proposed_loop_name < current_loop_name)
          {
            for (auto && entry : _component_name_to_loop_name)
              if (entry.second == current_loop_name)
                entry.second = proposed_loop_name;
          }
        }
      }
    }
  }

  // get the list of loops
  std::vector<std::string> loops;
  for (const auto & entry : _component_name_to_loop_name)
    if (std::find(loops.begin(), loops.end(), entry.second) == loops.end())
      loops.push_back(entry.second);

  // fill the map of loop name to model ID
  for (const auto & loop : loops)
  {
    // find a flow channel in this loop and get its model ID
    THM::FlowModelID model_id;
    bool found_model_id = false;
    for (const auto & component : _components)
    {
      const auto flow_chan_base_component =
          MooseSharedNamespace::dynamic_pointer_cast<FlowChannelBase>(component);
      if (flow_chan_base_component && (_component_name_to_loop_name[component->name()] == loop))
      {
        model_id = flow_chan_base_component->getFlowModelID();
        found_model_id = true;
        break;
      }
    }
    // set the value in the map or throw an error
    if (found_model_id)
      _loop_name_to_model_id[loop] = model_id;
    else
      logError("No FlowChannelBase-derived components were found in loop '", loop, "'");
  }
}

void
Simulation::printComponentLoops() const
{
  // get the list of loops
  std::vector<std::string> loops;
  for (auto && entry : _component_name_to_loop_name)
    if (std::find(loops.begin(), loops.end(), entry.second) == loops.end())
      loops.push_back(entry.second);

  // for each loop
  Moose::out << "\nListing of component loops:" << std::endl;
  for (unsigned int i = 0; i < loops.size(); i++)
  {
    Moose::out << "\n  Loop " << i + 1 << ":" << std::endl;

    // print out each component in the loop
    for (auto && entry : _component_name_to_loop_name)
      if (entry.second == loops[i])
        Moose::out << "    " << entry.first << std::endl;
  }
  Moose::out << std::endl;
}

void
Simulation::addSimVariable(bool nl, const VariableName & name, FEType type, Real scaling_factor)
{
  if (_vars.find(name) == _vars.end())
  {
    VariableInfo vi;
    vi._nl = nl;
    vi._type = type;
    vi._scaling_factor = scaling_factor;
    _vars[name] = vi;
  }
  else
  {
    VariableInfo & vi = _vars[name];
    if (vi._type != type)
      mooseError(
          "A component is trying to add variable of the same name but with different order/type");
  }
}

void
Simulation::addSimVariable(bool nl,
                           const VariableName & name,
                           FEType type,
                           const std::vector<SubdomainName> & subdomain_names,
                           Real scaling_factor /* = 1.*/)
{
  if (_vars.find(name) == _vars.end())
  {
    VariableInfo vi;
    vi._nl = nl;
    vi._type = type;
    for (auto && sdn : subdomain_names)
      vi._subdomain.insert(sdn);
    vi._scaling_factor = scaling_factor;
    _vars[name] = vi;
  }
  else
  {
    VariableInfo & vi = _vars[name];
    if (vi._type != type)
      mooseError(
          "A component is trying to add variable of the same name but with different order/type");
    for (auto && sdn : subdomain_names)
      vi._subdomain.insert(sdn);
  }
}

void
Simulation::addControl(const std::string & type, const std::string & name, InputParameters params)
{
  params.addPrivateParam<FEProblemBase *>("_fe_problem_base", &_fe_problem);
  std::shared_ptr<Control> control = _factory.create<Control>(type, name, params);
  _fe_problem.getControlWarehouse().addObject(control);
}

void
Simulation::addSimInitialCondition(const std::string & type,
                                   const std::string & name,
                                   InputParameters params)
{
  if (hasInitialConditionsFromFile())
    return;

  if (_ics.find(name) == _ics.end())
  {
    ICInfo ic(type, params);
    _ics[name] = ic;
  }
  else
    mooseError("Initial condition with name '", name, "' already exists.");
}

void
Simulation::addConstantIC(const VariableName & var_name,
                          Real value,
                          const std::vector<SubdomainName> & block_names)
{
  if (hasInitialConditionsFromFile())
    return;

  std::string blk_str = block_names[0];
  for (unsigned int i = 1; i < block_names.size(); i++)
    blk_str += ":" + block_names[i];

  std::string class_name = "ConstantIC";
  InputParameters params = _factory.getValidParams(class_name);
  params.set<VariableName>("variable") = var_name;
  params.set<Real>("value") = value;
  params.set<std::vector<SubdomainName>>("block") = block_names;
  addSimInitialCondition(class_name, genName(var_name, blk_str, "ic"), params);
}

void
Simulation::addFunctionIC(const VariableName & var_name,
                          const std::string & func_name,
                          const std::vector<SubdomainName> & block_names)
{
  if (hasInitialConditionsFromFile())
    return;

  std::string blk_str = block_names[0];
  for (unsigned int i = 1; i < block_names.size(); i++)
    blk_str += ":" + block_names[i];

  std::string class_name = "FunctionIC";
  InputParameters params = _factory.getValidParams(class_name);
  params.set<VariableName>("variable") = var_name;
  params.set<std::vector<SubdomainName>>("block") = block_names;
  params.set<FunctionName>("function") = func_name;
  addSimInitialCondition(class_name, genName(var_name, blk_str, "ic"), params);
}

void
Simulation::addConstantScalarIC(const VariableName & var_name, Real value)
{
  if (hasInitialConditionsFromFile())
    return;

  std::string class_name = "ScalarConstantIC";
  InputParameters params = _factory.getValidParams(class_name);
  params.set<VariableName>("variable") = var_name;
  params.set<Real>("value") = value;
  addSimInitialCondition(class_name, genName(var_name, "ic"), params);
}

void
Simulation::addComponentScalarIC(const VariableName & var_name, const std::vector<Real> & value)
{
  if (hasInitialConditionsFromFile())
    return;

  std::string class_name = "ScalarComponentIC";
  InputParameters params = _factory.getValidParams(class_name);
  params.set<VariableName>("variable") = var_name;
  params.set<std::vector<Real>>("values") = value;
  addSimInitialCondition(class_name, genName(var_name, "ic"), params);
}

void
Simulation::addVariables()
{
  Transient * trex = dynamic_cast<Transient *>(getApp().getExecutioner());
  if (trex)
  {
    Moose::TimeIntegratorType ti_type = trex->getTimeScheme();
    // This is only needed for the listed time integrators that are using the original approach to
    // explicit integration in MOOSE.  Currently, the new time integrators like
    // ActuallyExplicitEuler do not need the implicit flag to be set.
    if (ti_type == Moose::TI_EXPLICIT_TVD_RK_2 || ti_type == Moose::TI_EXPLICIT_MIDPOINT ||
        ti_type == Moose::TI_EXPLICIT_EULER)
      _implicit_time_integration = false;
  }

  if (_components.size() == 0)
    return;

  // let all components add their variables
  for (auto && comp : _components)
    comp->addVariables();

  // pass the variables to MOOSE
  for (auto && v : _vars)
  {
    VariableName name = v.first;
    if (name.size() > THM::MAX_VARIABLE_LENGTH)
      mooseError(
          "Variable name '", name, "' is too long. The limit is ", THM::MAX_VARIABLE_LENGTH, ".");

    VariableInfo & vi = v.second;

    if (vi._type.family != SCALAR)
    {
      auto order = AddVariableAction::getNonlinearVariableOrders();
      order = Utility::enum_to_string<Order>(vi._type.order);
      auto family = AddVariableAction::getNonlinearVariableFamilies();
      family = Utility::enum_to_string(vi._type.family);

      auto var_type = "MooseVariable";
      InputParameters params = _factory.getValidParams(var_type);
      params.set<MooseEnum>("order") = order;
      params.set<MooseEnum>("family") = family;
      if (!vi._subdomain.empty())
      {
        std::vector<SubdomainName> subdomains(vi._subdomain.begin(), vi._subdomain.end());
        params.set<std::vector<SubdomainName>>("block") = subdomains;
      }

      if (vi._nl)
      {
        params.set<std::vector<Real>>("scaling") = {vi._scaling_factor};
        _fe_problem.addVariable(var_type, name, params);
      }
      else
        _fe_problem.addAuxVariable(var_type, name, params);
    }
  }

  // pass the scalar variables to MOOSE
  for (auto && v : _vars)
  {
    const VariableName & name = v.first;
    if (name.size() > THM::MAX_VARIABLE_LENGTH)
      mooseError(
          "Variable name '", name, "' is too long. The limit is ", THM::MAX_VARIABLE_LENGTH, ".");
    const VariableInfo & vi = v.second;

    if (vi._type.family == SCALAR)
    {
      auto order = AddVariableAction::getNonlinearVariableOrders();
      order = Utility::enum_to_string<Order>(vi._type.order);
      auto family = AddVariableAction::getNonlinearVariableFamilies();
      family = Utility::enum_to_string(vi._type.family);

      auto var_type = "MooseVariableScalar";
      InputParameters params = _factory.getValidParams(var_type);
      params.set<MooseEnum>("order") = order;
      params.set<MooseEnum>("family") = family;

      if (vi._nl)
      {
        params.set<std::vector<Real>>("scaling") = {vi._scaling_factor};
        _fe_problem.addVariable(var_type, name, params);
      }
      else
        _fe_problem.addAuxVariable(var_type, name, params);
    }
  }

  if (hasInitialConditionsFromFile())
    setupInitialConditionsFromFile();
  else
    setupInitialConditionObjects();
}

void
Simulation::setupInitialConditionsFromFile()
{
  const UserObjectName suo_name = genName("thm", "suo");
  {
    const std::string class_name = "SolutionUserObject";
    InputParameters params = _factory.getValidParams(class_name);
    params.set<MeshFileName>("mesh") = _pars.get<FileName>("initial_from_file");
    params.set<std::string>("timestep") = _pars.get<std::string>("initial_from_file_timestep");
    _fe_problem.addUserObject(class_name, suo_name, params);
  }

  for (auto && v : _vars)
  {
    const VariableName & var_name = v.first;
    VariableInfo & vi = v.second;

    if (vi._type.family == SCALAR)
    {
      std::string class_name = "ScalarSolutionInitialCondition";
      InputParameters params = _factory.getValidParams(class_name);
      params.set<VariableName>("variable") = var_name;
      params.set<VariableName>("from_variable") = var_name;
      params.set<UserObjectName>("solution_uo") = suo_name;
      _fe_problem.addInitialCondition(class_name, genName(var_name, "ic"), params);
    }
    else
    {
      std::string class_name = "SolutionInitialCondition";
      InputParameters params = _factory.getValidParams(class_name);
      params.set<VariableName>("variable") = var_name;
      params.set<VariableName>("from_variable") = var_name;
      params.set<UserObjectName>("solution_uo") = suo_name;
      if (vi._subdomain.size() > 0)
      {
        std::vector<SubdomainName> subdomains(vi._subdomain.begin(), vi._subdomain.end());
        params.set<std::vector<SubdomainName>>("block") = subdomains;
      }
      _fe_problem.addInitialCondition(class_name, genName(var_name, "ic"), params);
    }
  }
}

void
Simulation::setupInitialConditionObjects()
{
  for (auto && i : _ics)
  {
    const std::string & name = i.first;
    ICInfo & ic = i.second;
    _fe_problem.addInitialCondition(ic._type, name, ic._params);
  }
}

void
Simulation::addMooseObjects()
{
  for (auto && comp : _components)
    comp->addMooseObjects();
}

void
Simulation::addRelationshipManagers()
{
  {
    const std::string class_name = "AugmentSparsityBetweenElements";
    auto params = _factory.getValidParams(class_name);
    params.set<Moose::RelationshipManagerType>("rm_type") =
        Moose::RelationshipManagerType::ALGEBRAIC | Moose::RelationshipManagerType::GEOMETRIC;
    params.set<std::string>("for_whom") = _fe_problem.name();
    params.set<MooseMesh *>("mesh") = &_mesh;
    params.set<std::map<dof_id_type, std::vector<dof_id_type>> *>("_elem_map") =
        &_sparsity_elem_augmentation;
    auto rm = _factory.create<RelationshipManager>(class_name, "thm:sparsity_btw_elems", params);
    if (!_app.addRelationshipManager(rm))
      _factory.releaseSharedObjects(*rm);
  }
}

void
Simulation::setupCoordinateSystem()
{
  MultiMooseEnum coord_types("XYZ RZ RSPHERICAL");
  std::vector<SubdomainName> blocks;

  for (auto && comp : _components)
  {
    GeometricalComponent * gc = dynamic_cast<GeometricalComponent *>(comp.get());
    if (gc != NULL && gc->parent() == nullptr)
    {
      const std::vector<SubdomainName> & subdomains = gc->getSubdomainNames();
      const std::vector<Moose::CoordinateSystemType> & coord_sys = gc->getCoordSysTypes();

      for (unsigned int i = 0; i < subdomains.size(); i++)
      {
        blocks.push_back(subdomains[i]);
        // coord_types.push_back("XYZ");
        coord_types.push_back(coord_sys[i] == Moose::COORD_RZ ? "RZ" : "XYZ");
      }
    }
  }
  _fe_problem.setCoordSystem(blocks, coord_types);

  // RZ geometries are always aligned with x-axis
  MooseEnum rz_coord_axis("X=0 Y=1", "X");
  _fe_problem.setAxisymmetricCoordAxis(rz_coord_axis);
}

void
Simulation::setupMesh()
{
  if (_components.size() == 0)
    return;

  setupCoordinateSystem();
}

void
Simulation::couplingMatrixIntegrityCheck() const
{
  if (!_fe_problem.shouldSolve())
    return;

  const TimeIntegrator * ti = _fe_problem.getNonlinearSystemBase().getTimeIntegrator();
  // Yes, this is horrible. Don't ask why...
  if ((dynamic_cast<const ExplicitTimeIntegrator *>(ti) != nullptr) ||
      (dynamic_cast<const ExplicitEuler *>(ti) != nullptr) ||
      (dynamic_cast<const ExplicitRK2 *>(ti) != nullptr) ||
      (dynamic_cast<const ExplicitTVDRK2 *>(ti) != nullptr))
    return;

  const CouplingMatrix * cm = _fe_problem.couplingMatrix();
  if (cm == nullptr)
    mooseError("Coupling matrix does not exists. Something really bad happened.");

  bool full = true;
  for (unsigned int i = 0; i < cm->size(); i++)
    for (unsigned int j = 0; j < cm->size(); j++)
      full &= (*cm)(i, j);

  if (!full)
    mooseError(
        "Single matrix preconditioning with full coupling is required to run. Please, check that "
        "your input file has the following preconditioning block:\n\n"
        "[Preconditioning]\n"
        "  [pc]\n"
        "    type = SMP\n"
        "    full = true\n"
        "  []\n"
        "[].\n");
}

void
Simulation::integrityCheck() const
{
  if (_components.size() == 0)
    return;

  if (_check_jacobian)
    return;

  // go over components and put flow channels into one "bucket"
  std::vector<Component *> flow_channels;
  for (auto && comp : _components)
  {
    auto flow_channel = dynamic_cast<FlowChannelBase *>(comp.get());
    if (flow_channel != nullptr)
      flow_channels.push_back(flow_channel);
  }

  // initialize number of connected flow channel inlets and outlets to zero
  std::map<std::string, unsigned int> flow_channel_inlets;
  std::map<std::string, unsigned int> flow_channel_outlets;
  for (auto && comp : flow_channels)
  {
    flow_channel_inlets[comp->name()] = 0;
    flow_channel_outlets[comp->name()] = 0;
  }

  // mark connections of any FlowConnection components
  for (const auto & comp : _components)
  {
    auto pc_comp = dynamic_cast<FlowConnection *>(comp.get());
    if (pc_comp != nullptr)
    {
      for (const auto & connection : pc_comp->getConnections())
      {
        if (connection._end_type == FlowConnection::IN)
          flow_channel_inlets[connection._geometrical_component_name]++;
        else if (connection._end_type == FlowConnection::OUT)
          flow_channel_outlets[connection._geometrical_component_name]++;
      }
    }
  }

  // finally, check that each flow channel has exactly one input and one output
  for (auto && comp : flow_channels)
  {
    if (flow_channel_inlets[comp->name()] == 0)
      logError("Component '", comp->name(), "' does not have connected inlet.");
    else if (flow_channel_inlets[comp->name()] > 1)
      logError("Multiple inlets specified for component '", comp->name(), "'.");

    if (flow_channel_outlets[comp->name()] == 0)
      logError("Component '", comp->name(), "' does not have connected outlet.");
    else if (flow_channel_outlets[comp->name()] > 1)
      logError("Multiple outlets specified for component '", comp->name(), "'.");
  }

  // let components check themselves
  for (auto && comp : _components)
    comp->executeCheck();

  if (_log.getNumberOfErrors() > 0)
  {
    if (processor_id() == 0)
    {
      Moose::err << COLOR_RED
                 << "Execution stopped, the following problems were found:" << COLOR_DEFAULT
                 << std::endl
                 << std::endl;
      _log.print();
      Moose::err << std::endl;
    }

    MPI_Finalize();
    exit(1);
  }

  if ((_log.getNumberOfWarnings() > 0) && (processor_id() == 0))
  {
    _log.print();
    Moose::err << std::endl;
  }
}

void
Simulation::controlDataIntegrityCheck()
{
  if (_check_jacobian)
    return;

  // check that control data are consistent
  for (auto && i : _control_data)
  {
    if (!i.second->getDeclared())
      logError("Control data '",
               i.first,
               "' was requested, but was not declared by any active control object.");
  }

  if (_log.getNumberOfErrors() > 0)
  {
    Moose::err << COLOR_RED
               << "Execution stopped, the following problems were found:" << COLOR_DEFAULT
               << std::endl
               << std::endl;
    _log.print();
    Moose::err << std::endl;
    MOOSE_ABORT;
  }

  auto & ctrl_wh = _fe_problem.getControlWarehouse()[EXEC_TIMESTEP_BEGIN];

  // initialize THM control objects
  for (auto && i : ctrl_wh.getObjects())
  {
    THMControl * ctrl = dynamic_cast<THMControl *>(i.get());
    if (ctrl != nullptr)
      ctrl->init();
  }

  for (auto && i : ctrl_wh.getObjects())
  {
    THMControl * ctrl = dynamic_cast<THMControl *>(i.get());
    // if it is a THM control
    if (ctrl != nullptr)
    {
      // get its dependencies on control data
      auto & cd_deps = ctrl->getControlDataDependencies();
      for (auto && cd_name : cd_deps)
      {
        ControlDataValue * cdv = _control_data[cd_name];
        // find out which control object built the control data
        std::string dep_name = cdv->getControl()->name();
        auto & deps = ctrl->getDependencies();
        // and if it is not in its dependency list, add it
        auto it = std::find(deps.begin(), deps.end(), dep_name);
        if (it == deps.end())
          deps.push_back(dep_name);
      }
    }
  }

  // Find all `TerminateControl`s and all their dependencies. Then add those
  // objects into TIMESTEP_END control warehouse
  MooseObjectWarehouse<Control> & ctrl_wh_tse =
      _fe_problem.getControlWarehouse()[EXEC_TIMESTEP_END];
  for (auto && i : ctrl_wh.getObjects())
  {
    if (TerminateControl * ctrl = dynamic_cast<TerminateControl *>(i.get()))
    {
      std::list<const THMControl *> l;
      l.push_back(ctrl);
      while (l.size() > 0)
      {
        const THMControl * ctrl = l.front();
        auto & cd_deps = ctrl->getControlDataDependencies();
        for (auto && cd_name : cd_deps)
        {
          ControlDataValue * cdv = _control_data[cd_name];
          l.push_back(cdv->getControl());
        }
        ctrl_wh_tse.addObject(ctrl_wh.getObject(ctrl->name()));
        l.pop_front();
      }
    }
  }
}

void
Simulation::run()
{
}

void
Simulation::addComponent(const std::string & type, const std::string & name, InputParameters params)
{
  std::shared_ptr<Component> comp = _factory.create<Component>(type, name, params);
  if (_comp_by_name.find(name) == _comp_by_name.end())
    _comp_by_name[name] = comp;
  else
    logError("Component with name '", name, "' already exists");
  _components.push_back(comp);
}

bool
Simulation::hasComponent(const std::string & name) const
{
  auto it = _comp_by_name.find(name);
  return (it != _comp_by_name.end());
}

void
Simulation::addClosures(const std::string & type, const std::string & name, InputParameters params)
{
  std::shared_ptr<ClosuresBase> obj_ptr = _factory.create<ClosuresBase>(type, name, params);
  if (_closures_by_name.find(name) == _closures_by_name.end())
    _closures_by_name[name] = obj_ptr;
  else
    logError("A closures object with the name '", name, "' already exists.");
}

bool
Simulation::hasClosures(const std::string & name) const
{
  return _closures_by_name.find(name) != _closures_by_name.end();
}

std::shared_ptr<ClosuresBase>
Simulation::getClosures(const std::string & name) const
{
  auto it = _closures_by_name.find(name);
  if (it != _closures_by_name.end())
    return it->second;
  else
    mooseError("The requested closures object '", name, "' does not exist.");
}

void
Simulation::addFileOutputter(const std::string & name)
{
  _outputters_all.push_back(name);
  _outputters_file.push_back(name);
}

void
Simulation::addScreenOutputter(const std::string & name)
{
  _outputters_all.push_back(name);
  _outputters_screen.push_back(name);
}

std::vector<OutputName>
Simulation::getOutputsVector(const std::string & key) const
{
  std::string key_lowercase = key;
  std::transform(key_lowercase.begin(), key_lowercase.end(), key_lowercase.begin(), ::tolower);

  std::vector<OutputName> outputs;
  if (key_lowercase == "none")
    outputs.push_back("none"); // provide non-existent name, so it does not get printed out
  else if (key_lowercase == "screen")
    outputs = _outputters_screen;
  else if (key_lowercase == "file")
    outputs = _outputters_file;
  else if (key_lowercase == "both")
    outputs = _outputters_all;
  else
    mooseError("The outputs vector key '" + key_lowercase + "' is invalid");

  return outputs;
}

bool
Simulation::hasInitialConditionsFromFile()
{
  return _pars.isParamValid("initial_from_file");
}

void
Simulation::advanceState()
{
  for (auto && i : _control_data)
    i.second->copyValuesBack();
}
