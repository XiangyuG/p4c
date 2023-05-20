import json

import sys

'''
{
        "Node_ID" : 1339,
        "Node_Type" : "Type_Struct",
        "name" : "headers",
        "declid" : 64,
        "annotations" : {
          "Node_ID" : 4
        },
        "typeParameters" : {
          "Node_ID" : 1325,
          "Node_Type" : "TypeParameters",
          "parameters" : {
            "Node_ID" : 1326,
            "Node_Type" : "IndexedVector<Type_Var>",
            "vec" : [],
            "declarations" : {}
          }
        },
        "fields" : {
          "Node_ID" : 1340,
          "Node_Type" : "IndexedVector<StructField>",
          "vec" : [
            {
              "Node_ID" : 1333,
              "Node_Type" : "StructField",
              "name" : "vlan_tag_",
              "declid" : 191,
              "annotations" : {
                "Node_ID" : 4
              },
              "type" : {
                "Node_ID" : 1332,
                "Node_Type" : "Type_Stack",
                "elementType" : {
                  "Node_ID" : 1329,
                  "Node_Type" : "Type_Name",
                  "path" : {
                    "Node_ID" : 1328,
                    "Node_Type" : "Path",
                    "name" : "vlan_tag_t",
                    "absolute" : false
                  }
                },
                "size" : {
                  "Node_ID" : 1331,
                  "Node_Type" : "Constant",
                  "type" : {
                    "Node_ID" : 1330,
                    "Node_Type" : "Type_InfInt",
                    "declid" : 43
                  },
                  "value" : 2,
                  "base" : 10
                }
              }
            },
            {
              "Node_ID" : 1336,
              "Node_Type" : "StructField",
              "name" : "ethernet",
              "declid" : 192,
              "annotations" : {
                "Node_ID" : 4
              },
              "type" : {
                "Node_ID" : 1335,
                "Node_Type" : "Type_Name",
                "path" : {
                  "Node_ID" : 1334,
                  "Node_Type" : "Path",
                  "name" : "ethernet_t",
                  "absolute" : false
                }
              }
            }
          ],
          "declarations" : {
            "vlan_tag_" : {
              "Node_ID" : 1333,
              "Node_Type" : "StructField",
              "name" : "vlan_tag_",
              "declid" : 191,
              "annotations" : {
                "Node_ID" : 4
              },
              "type" : {
                "Node_ID" : 1332
              }
            },
            "ethernet" : {
              "Node_ID" : 1336,
              "Node_Type" : "StructField",
              "name" : "ethernet",
              "declid" : 192,
              "annotations" : {
                "Node_ID" : 4
              },
              "type" : {
                "Node_ID" : 1335
              }
            }
          }
        }
      }
'''

def parse_type_header(dic):
    # Parse the header definition from json file
    assert dic['Node_Type'] == 'Type_Header', 'wrong node type (should be Type_Header)'
    ret_dic = {}
    # extract variable name
    var_name = dic['name']
    ret_dic['var_name'] = var_name
    # extract all 
    ret_dic['fields'] = []
    fields = dic['fields']
    field_vec = fields['vec']
    for mem in field_vec:
        sub_var_name = mem['name']
        assert mem['type']['Node_Type'] == 'Type_Bits', "Assume all members of a packets are bit<>"
        bit_len = mem['type']['size']
        ret_dic['fields'].append({'sub_var_name':sub_var_name, 'bit_len':bit_len})
    # ret_dic['var_name] = var_name; ret_dic['fields'] = [{'sub_var_name': sub_var_name, 'bit_len': bit_len}, {}]
    return ret_dic  

def parse_type_struct(dic):
    # Parse the header definition from json file
    assert dic['Node_Type'] == 'Type_Struct', 'wrong node type (should be Type_Struct)'
    ret_dic = {}
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

    for i in range(len(vec)):
        node_type = vec[i]['Node_Type']
        if node_type == 'Type_Header':
            type_header_list.append(parse_type_header(vec[i]))
        elif node_type == 'TypeStruct':
            type_header_list.append(parse_type_struct(vec[i]))

if __name__ == '__main__':
    main(sys.argv)