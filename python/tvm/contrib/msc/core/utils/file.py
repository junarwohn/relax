# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
"""tvm.contrib.msc.core.utils.file"""

import os
import shutil
import tempfile
import types
from typing import List
from importlib.machinery import SourceFileLoader

from .namespace import MSCFramework
from .register import get_registered_func


class MSCDirectory(object):
    """Create a directory manager for MSC"""

    def __init__(self, path: str = None, keep_history: bool = True, cleanup: bool = False):
        self._path = os.path.abspath(path or tempfile.mkdtemp())
        self._cleanup = cleanup
        self._cwd = os.getcwd()
        if os.path.isdir(self._path) and not keep_history:
            shutil.rmtree(self._path)
        if not os.path.isdir(self._path):
            os.mkdir(self._path)

    def __str__(self):
        return "{}(Cleanup: {}): {} Files".format(self._path, self._cleanup, len(self.listdir()))

    def __enter__(self):
        os.chdir(self._path)
        return self

    def __exit__(self, exception_type, exception_value, traceback):
        os.chdir(self._cwd)
        self.clean_up()

    def __del__(self):
        self.clean_up()

    def clean_up(self):
        """Clean up the dir"""

        if self._cleanup and os.path.isdir(self._path):
            shutil.rmtree(self._path)

    def add_file(self, name: str, contains: str) -> str:
        """Add a file under the folder

        Parameters
        ----------
        name: str
            The name of the file.
        contains: str
            The contains of the file.

        Returns
        -------
        path: str
            The abs file path.
        """

        file_path = self.relpath(name)
        with open(file_path, "w") as f:
            f.write(contains)
        return file_path

    def move_file(self, src_file: str, dst_folder: object, dst_file: str = None):
        """Move a file to another folder

        Parameters
        ----------
        src_file: str
            The name of the source file.
        dst_folder: MSCDirectory
            The target folder.
        dst_file: str
            The target file name.

        Returns
        -------
        path: str
            The abs file path.
        """

        src_path = os.path.join(self.relpath(src_file))
        assert os.path.isfile(src_path), "Source file {} not exist".format(src_path)
        dst_path = dst_folder.relpath(dst_file or src_file)
        os.rename(src_path, dst_path)
        return dst_path

    def copy_file(self, src_file: str, dst_folder: object, dst_file: str = None):
        """Copy a file to another folder

        Parameters
        ----------
        src_file: str
            The name of the source file.
        dst_folder: MSCDirectory
            The target folder.
        dst_file: str
            The target file name.

        Returns
        -------
        path: str
            The abs file path.
        """

        src_path = os.path.join(self.relpath(src_file))
        assert os.path.isfile(src_path), "Source file {} not exist".format(src_path)
        dst_path = dst_folder.relpath(dst_file or src_file)
        shutil.copy2(src_path, dst_path)
        return dst_path

    def create_dir(self, name: str, keep_history: bool = True, cleanup: bool = False) -> object:
        """Add a dir under the folder

        Parameters
        ----------
        name: str
            The name of the file.
        keep_history: bol
            Whether to keep history.
        cleanup: bool
            Whether to clean up before exit.


        Returns
        -------
        dir: MSCDirectory
            The created dir.
        """

        dir_path = self.relpath(name)
        if os.path.isfile(dir_path):
            os.remove(dir_path)
        return self.__class__(dir_path, keep_history=keep_history, cleanup=cleanup)

    def relpath(self, name: str) -> str:
        """Relative path in dir

        Parameters
        ----------
        name: str
            The name of the file.

        Returns
        -------
        path: str
            The concatenated path.
        """

        return os.path.join(self._path, name)

    def listdir(self) -> List[str]:
        """List contents in the dir.

        Returns
        -------
        names: list
            The content of directory
        """

        return os.listdir(self._path)

    def destory(self):
        """Destory the dir."""

        if os.path.isdir(self._path):
            shutil.rmtree(self._path)

    @property
    def path(self):
        return self._path


def msc_dir(path: str = None, keep_history: bool = True, cleanup: bool = False) -> MSCDirectory:
    """Create MSCDirectory

    Parameters
    ----------
    path: str
        The path of the dir.
    keep_history: bool
        Whether to remove files before start.
    cleanup: bool
        Whether to clean up before exit.

    Returns
    -------
    dir: MSCDirectory
        The created dir.
    """

    return MSCDirectory(path, keep_history, cleanup)


def load_callable(name: str, framework: str = MSCFramework.MSC) -> callable:
    """Load a callable  object.

    Parameters
    ----------
    name: string
        The name of the registered func or path:f_name str.
    framework: string
        Should be from MSCFramework.

    Returns
    -------
    func: callable
        The function.
    """

    func = get_registered_func(name, framework)
    if func:
        return func
    if ".py:" in name:
        path, func_name = name.split(":")
        loader = SourceFileLoader(path.replace(".py", ""), path)
        mod = types.ModuleType(loader.name)
        loader.exec_module(mod)
        return getattr(mod, func_name)
    raise Exception("Func {} is neighter registered nor path.py:name string")
