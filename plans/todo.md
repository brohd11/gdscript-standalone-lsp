
- some issues with const inheritance - dock_manager.gd, UFile in the inner class
- u_node.gd, has issues with global scope and object methods

- const Global = GlobalClass # this is not erroring, I think it is not valid though

var re-assignment not fully tested?:
- var mybool:bool = false; mybool = 2
- this doesn't fail, but assigning a Vector to it does, is that right?