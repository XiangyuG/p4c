import json

import sys

def parse_type_header(dic):
    # Parse the header definition from json file
    assert dic['Node_Type'] == 'Type_Header', 'wrong node type (should be Type_Header)'
    ret_dic = {}
    # extract variable name
    var_name = dic['name']
    ret_dic['var_name'] = var_name
    # extract all sub variables' name
    ret_dic['fields'] = []
    fields = dic['fields']
    field_vec = fields['vec']
    for mem in field_vec:
        sub_var_name = mem['name']
        assert mem['type']['Node_Type'] == 'Type_Bits', "Assume all members of a packets are bit<>"
        bit_len = mem['type']['size']
        ret_dic['fields'].append({'sub_var_name':sub_var_name, 'bit_len':bit_len})
    # ret_dic['var_name] = var_name; ret_dic['fields'] = [{'sub_var_name': sub_var_name, 'bit_len': bit_len}, {}]
    # print(ret_dic)
    return ret_dic  

def parse_type_struct(dic):
    # Parse the struct definition from json file
    assert dic['Node_Type'] == 'Type_Struct', 'wrong node type (should be Type_Struct)'
    ret_dic = {}
    var_name = dic['name']
    # Currently we do not need to collect info for standard_metadata_t
    # TODO: deal with standard_metadata_t in the future
    if var_name == "standard_metadata_t":
        return
    ret_dic['var_name'] = var_name
    # extract all sub variables' name
    ret_dic['fields'] = []
    fields = dic['fields']
    field_vec = fields['vec']
    for mem in field_vec:
        sub_var_name = mem['name']
        if mem['type']['Node_Type'] == 'Type_Name':
            type = mem['type']['path']['name']
        elif mem['type']['Node_Type'] == 'Type_Stack':
            type = mem['type']['elementType']['path']['name']
        # TODO: consider other Node_Type
        size = 1
        if 'size' in mem['type']:
            size = mem['type']['size']['value']
        ret_dic['fields'].append({'sub_var_name':sub_var_name, 'type':type, 'size':size})
    return ret_dic

def collect_control_parameter(dic):
    # Parse the parameters in P4 control block
    ret_list = []
    assert 'applyParams' in dic, "Do not have applyParams in dic"
    applyParams = dic['applyParams']
    parameters = applyParams['parameters']
    parameters_vec = parameters['vec']
    for mem in parameters_vec:
        para_name = mem['name']
        direction = mem['direction'] # TODO: find a better way to deal with the situation where there is no direction there
        type = mem['type']['path']['name']
        ret_list.append({'direction':direction,'type':type,'para_name':para_name})
    return ret_list

def parseKeyElement(dic):
    match_key_list = []
    keyElements_vec = dic['keyElements']['vec']
    
    for keyElement in keyElements_vec:
        # Node_Type = keyElement['Node_Type']
        # TODO: find a better way to get match key, this is too domain-specific
        annotations_vec = keyElement['annotations']['annotations']['vec']
        match_key = annotations_vec[0]['expr']['vec'][0]['value']
        # get match_type
        match_type = keyElement['matchType']['path']['name']
        match_key_list.append({'match_key':match_key, 'match_type':match_type})
    # print("match_key_list =", match_key_list)
    return match_key_list

def parse_P4Table(dic):
    # Parse the p4 table definition from json file
    ret_dic = {}
    table_name = dic['name']
    properties = dic['properties']
    properties_vec = properties['properties']['vec']
    for mem in properties_vec:
        name = mem['name']
        if name == 'key':
            # collect key info
            key_list = parseKeyElement(mem['value'])
        elif name == 'actions':
            # TODO: collect actions info
            continue
        elif name == 'size':
            # TODO: collect size info
            if mem['value']['expression']['Node_Type'] == 'Constant':
                key_size = mem['value']['expression']['value']
            else:
                # set the key_size to be 1 by default
                key_size = 1
            continue
        elif name == 'default_action':
            # TODO: collect default_action info
            continue
        # print(name)
        
    ret_dic['table_name'] = table_name
    ret_dic['key_list'] = key_list
    ret_dic['key_size'] = key_size
    # print(ret_dic)
    return ret_dic

def get_left_component(dic):
    Node_Type = dic['Node_Type']
    if Node_Type == 'Member':
        member = dic['member']
        expr = dic['expr']
        expr_node_type = expr['Node_Type']
        assert expr_node_type == 'PathExpression', 'We assume the LHS of expr is PathExpression'
        path = expr['path']
        var_name = path['name']
        return var_name + '.' + member
    else:
        assert False, 'New Node_Type (other than Member) in LHS which is currently not supported'
    return ""

def get_right_component(dic):
    Node_Type = dic['Node_Type']
    assert Node_Type == 'PathExpression', "Only deal with PathExpression in Node_Type of RHS of the action"
    return dic['path']['name']

def parse_P4Action(dic):
    # Parse the p4 action definition from json file
    ret_dic = {}
    # Get action name
    action_name = dic['name']
    # Get parameters in the action
    parameters_list = []
    parameters = dic['parameters']
    parameters_vec = parameters['parameters']['vec']
    for mem in parameters_vec:
        parameter_name = mem['name']
        parameter_type = mem['type']['Node_Type']
        assert parameter_type == 'Type_Bits', "Currently support Type_Bits as the type for parameters in P4action"
        bit_len = mem['type']['size']
        parameters_list.append({'parameter_name':parameter_name, 'bit_len':bit_len})
    # Get body of the action
    action_body_list = []
    body = dic['body']
    components = body['components']
    components_vec = components['vec']
    for mem in components_vec:
        Node_Type = mem['Node_Type']
        if Node_Type == 'AssignmentStatement':
            op = '='
            left = get_left_component(mem['left'])
            right = get_right_component(mem['right'])
            action_body_list.append({'left':left, 'op':op, 'right':right})
    ret_dic['action_name'] = action_name
    ret_dic['parameters_list'] = parameters_list
    ret_dic['action_body_list'] = action_body_list
    # print("parse_P4Action:", ret_dic)
    return ret_dic

def parse_P4Control(dic):
    # Parse the p4 control definition from json file
    assert dic['Node_Type'] == 'P4Control', 'wrong node type (should be P4Control)'
    ret_dic = {}
    ctl_name = dic['name']
    ret_dic['control name'] = ctl_name
    # TODO: Deal with "applyParams"
    for key in dic:
        if key == 'type':
            # collect the parameters in control block
            control_parameter_list = collect_control_parameter(dic[key])
            ret_dic['ctrl_parameters'] = control_parameter_list
        elif key == 'controlLocals':
            # collect Table & Action info from control block
            control_declaration_vec = dic[key]['vec']
            action_list = []
            table_list = []
            for mem in control_declaration_vec:
                Node_Type = mem['Node_Type']
                if Node_Type == 'P4Action':
                    action_name = mem['name']
                    if action_name.find('NoAction') != -1:
                        continue
                    # DONE: collect P4Action
                    action_list.append(parse_P4Action(mem))
                elif Node_Type == 'P4Table':
                    # TODO: collect P4Table
                    table_dic = parse_P4Table(mem)
                    table_list.append(table_dic)
            ret_dic['action_list'] = action_list
            ret_dic['table_list'] = table_list
    
    # print("parse_P4Control", ret_dic)
    return ret_dic

def main(argv):
    if len(argv) != 2:
        print("Usage: python3", argv[0], "<json filename>")
        sys.exit(1)
    json_filename = argv[1]
    # Load json file ref: https://www.freecodecamp.org/news/loading-a-json-file-in-python-how-to-read-and-parse-json/
    with open(json_filename) as user_file:
        parsed_json = json.load(user_file)
    assert parsed_json['Node_Type'] == 'P4Program', "input should be the json file of a P4 program"
    # all P4 file contents are included in the object part
    objects_of_input_p4 = parsed_json['objects']
    vec = objects_of_input_p4['vec']
    
    type_header_list = [] # record the info from the Type_Header node
    type_struct_list = [] # record the info from the Type_Struct node
    P4Control_list = [] # record the info from the P4Table node

    for i in range(len(vec)):
        node_type = vec[i]['Node_Type']
        # print(node_type)
        if node_type == 'Type_Header':
            type_header_list.append(parse_type_header(vec[i]))
        elif node_type == 'Type_Struct':
            type_struct_list.append(parse_type_struct(vec[i]))
        elif node_type == 'P4Parser':
            # TODO: deal with P4 parser, currently we do not need to deal with that
            continue
        elif node_type == 'Declaration_Instance':
            # TODO: deal with Declaration_Instance, currently we do not need to deal with that
            # because this only shows the general structure of this P4 program
            continue
        elif node_type == 'P4Control':
            P4Control_list.append(parse_P4Control(vec[i]))
    # print("type_header_list =", type_header_list)
    # print("type_struct_list =", type_struct_list)
    for v in P4Control_list:
        print(v)
    # print("P4Control_list =", P4Control_list)
if __name__ == '__main__':
    main(sys.argv)